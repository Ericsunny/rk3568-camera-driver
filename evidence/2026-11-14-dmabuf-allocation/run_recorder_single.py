#!/usr/bin/env python3
"""Bounded board-only probe runner; footage remains inside its private run dir."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import subprocess
import sys
import time

ALERT = re.compile(r'crc errors|err[12]:|kernel panic|\boops\b|\bBUG:|'
                   r'\btimeout\b|timed out|hung task|soft lockup|hard lockup', re.I)
CSI = re.compile(r'rockchip-mipi-csi2 fdfb0000\.mipi-csi2: stream '
                 r'(?:(?:on|off), src_sd: (?:[0-9a-f]+|\(ptrval\)), '
                 r'sd_name:rockchip-csi2-dphy2|ON|OFF)')


def kernel_alert(raw):
    text = raw.decode('utf-8', 'replace')
    head, sep, msg = text.partition(';')
    if not sep:
        raise RuntimeError('invalid kernel log record')
    known = CSI.fullmatch(msg.splitlines()[0]) is not None
    return bool(ALERT.search(msg)) or ((int(head.split(',')[0]) & 7) <= 3 and not known), text


def collect(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    return {'argv': cmd, 'rc': p.returncode, 'output': p.stdout.decode('utf-8', 'replace')}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('binary')
    p.add_argument('device')
    p.add_argument('width', type=int)
    p.add_argument('height', type=int)
    p.add_argument('fps', type=int)
    p.add_argument('mode', choices=('copy', 'dmabuf'))
    p.add_argument('--detect', action='store_true')
    p.add_argument('--rtsp', type=int, default=0)
    p.add_argument('--fault', default='')
    p.add_argument('--stop-active', action='store_true')
    p.add_argument('out')
    a = p.parse_args()
    out = Path(a.out)
    out.mkdir(mode=0o700)
    command = [a.binary, '-d', a.device, '-w', str(a.width), '-h', str(a.height),
               '--fps', str(a.fps), '--gop', str(a.fps), '--kbps', '4000',
               '--pre', '2', '--post', '2', '--max-events', '1', '--ring-mb', '64',
               '--osd', '--encoder-input', a.mode, '--recover', '1',
               '-o', str(out / 'event')]
    if a.detect:
        # Exercise RGA/NPU; controlled manual event excludes scene-dependent triggers.
        command += ['--detect', '--det-score', '1.1']
    if a.rtsp:
        command += ['--rtsp', str(a.rtsp), '--rtsp-clients', '2']
    if a.fault:
        command += ['--fault-sim', a.fault]
    plan = {'argv': command, 'timeout_s': 90, 'D_limit_s': 3, 'trigger_after_s': 6, 'stop_active': a.stop_active,
            'pass': 'exit 0; triggered after preroll ready; matching returned inputs before QBUF in dmabuf; '
                    'both bitstreams fully decode and per-frame hashes match; '
                    'no kernel alert; 3A active; no remaining test process',
            'scope': 'isolated full recorder; manual event; detect threshold 1.1 when requested; no human detection acceptance'}
    (out / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')
    checks = {'3a': collect(['systemctl', 'is-active', 'rkaiq_3A']),
              'busy': collect(['fuser', a.device]),
              'format': collect(['v4l2-ctl', '-d', a.device, '--get-fmt-video']),
              'uname': collect(['uname', '-a'])}
    (out / 'precheck.json').write_text(json.dumps(checks, indent=2) + '\n')
    if checks['3a']['rc'] or checks['busy']['rc'] != 1:
        raise RuntimeError('precheck failed')
    child = None
    kfd = -1
    result = {'rc': None, 'stop': None, 'child_alive': False, 'start_utc': time.strftime('%FT%TZ', time.gmtime())}
    started = time.monotonic()
    try:
        kfd = os.open('/dev/kmsg', os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        os.lseek(kfd, 0, os.SEEK_END)
        with (out / 'kernel-new.log').open('w') as kernel, (out / 'run.log').open('wb') as log:
            rga_probes = 0
            def drain():
                nonlocal rga_probes
                while select.select([kfd], [], [], 0)[0]:
                    try:
                        raw = os.read(kfd, 65536)
                    except BlockingIOError:
                        break
                    except OSError as exc:
                        if exc.errno == errno.EPIPE:
                            raise RuntimeError('kernel records lost') from exc
                        raise
                    alert, text = kernel_alert(raw)
                    if (a.detect and text.partition(';')[2].splitlines()[0] == 'rga2: unknown ioctl cmd!'
                            and not ALERT.search(text.partition(';')[2])):
                        rga_probes += 1
                        alert = rga_probes > 1
                        result['rga_version_probe_count'] = rga_probes
                    kernel.write(text + '\n')
                    kernel.flush()
                    if alert:
                        raise RuntimeError('kernel alert: ' + text.strip())

            drain()
            usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            triggered = False
            term_sent = False
            child = subprocess.Popen(command, cwd=str(out), stdout=log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
            (out / 'producer.pid').write_text(str(child.pid) + '\n')
            d_since = {}
            while child.poll() is None:
                drain()
                now = time.monotonic()
                try:
                    tasks = list(Path('/proc/%d/task' % child.pid).iterdir())
                except FileNotFoundError:
                    tasks = []
                seen = set()
                for task in tasks:
                    try:
                        status = (task / 'status').read_text()
                    except (FileNotFoundError, ProcessLookupError):
                        continue
                    if re.search(r'^State:\s+D\s', status, re.M):
                        seen.add(task.name)
                        d_since.setdefault(task.name, now)
                        if now - d_since[task.name] >= 3:
                            raise RuntimeError('thread remained D for 3 seconds')
                d_since = {key: value for key, value in d_since.items() if key in seen}
                if not triggered and now - started >= 6:
                    if '预录就绪' in (out / 'run.log').read_text(errors='replace'):
                        child.send_signal(signal.SIGUSR1)
                        triggered = True
                        result['trigger_sent_s'] = round(now - started, 3)
                if triggered and a.stop_active and not term_sent and now - started >= result['trigger_sent_s'] + .6:
                    child.terminate()
                    term_sent = True
                if now - started > 90:
                    raise RuntimeError('probe timeout')
                time.sleep(.05)
            result['rc'] = child.returncode
            usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            result['user_s'] = usage_after.ru_utime - usage_before.ru_utime
            result['system_s'] = usage_after.ru_stime - usage_before.ru_stime
            result['maxrss_kb'] = usage_after.ru_maxrss
            result['triggered'] = triggered
            until = time.monotonic() + .5
            while time.monotonic() < until:
                drain()
                time.sleep(.05)
    except (OSError, ValueError, RuntimeError) as exc:
        result['stop'] = str(exc)
    finally:
        if child and child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                result['child_alive'] = True
        if kfd >= 0:
            os.close(kfd)
        result['elapsed_s'] = round(time.monotonic() - started, 3)
        result['rc'] = child.poll() if child else None
        result['3a_after'] = collect(['systemctl', 'is-active', 'rkaiq_3A'])
        result['busy_after'] = collect(['fuser', a.device])
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), flush=True)
    return 0 if result['rc'] == 0 and result.get('triggered') and not result['stop'] and not result['child_alive'] else 1


if __name__ == '__main__':
    sys.exit(main())
