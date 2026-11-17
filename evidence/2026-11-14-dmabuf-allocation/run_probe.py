#!/usr/bin/env python3
"""Bounded board-only probe runner; footage remains inside its private run dir."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
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
    p.add_argument('frames', type=int)
    p.add_argument('out')
    p.add_argument('--allow-rga', action='store_true')
    a = p.parse_args()
    out = Path(a.out)
    out.mkdir(mode=0o700)
    command = [a.binary, a.device, str(a.width), str(a.height), str(a.fps), str(a.frames)]
    plan = {'argv': command, 'timeout_s': 90, 'D_limit_s': 3,
            'pass': 'exit 0; matching returned inputs before QBUF; requested frame count; '
                    'both bitstreams fully decode and per-frame hashes match; '
                    'no kernel alert; 3A active; no remaining test process',
            'scope': 'isolated same-frame two-encoder probe, no service performance claim'}
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
                    if (a.allow_rga and text.partition(';')[2].splitlines()[0] == 'rga2: unknown ioctl cmd!'
                            and not ALERT.search(text.partition(';')[2])):
                        rga_probes += 1
                        alert = rga_probes > 1
                        result['rga_version_probe_count'] = rga_probes
                    kernel.write(text + '\n')
                    kernel.flush()
                    if alert:
                        raise RuntimeError('kernel alert: ' + text.strip())

            drain()
            child = subprocess.Popen(command, cwd=str(out), stdout=log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
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
                if now - started > 90:
                    raise RuntimeError('probe timeout')
                time.sleep(.05)
            result['rc'] = child.returncode
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
    return 0 if result['rc'] == 0 and not result['stop'] and not result['child_alive'] else 1


if __name__ == '__main__':
    sys.exit(main())
