#!/usr/bin/env python3
"""Bounded dual-detection observation with process/ISP progress sampling."""
import datetime
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

APP = Path('/root/camera-integration-20261110-dual-app')
OUT = Path('/root/camera-integration-20261110-build/dual-progress-watch-20261110')
RUN = OUT / 'run'


def command(*args):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=8)


def read(path):
    try:
        return Path(path).read_text(errors='replace')
    except OSError:
        return ''


def main():
    OUT.mkdir(mode=0o700)
    plan = {
        'test': 'dual detect progress observation only; no clients',
        'command': ['python3', str(APP / 'tools/run_dual_camera.py'), 'detect', str(RUN)],
        'duration_s': 22, 'sample_interval_s': 0.25,
        'stop_conditions': ['D state continuously >= 3 seconds for either recorder',
                            'no ISP mainpath frame advance for 5 seconds after first frame',
                            'missing first frame after 12 seconds', 'early supervisor exit'],
        'scope': 'camera images and event videos stay on board; 3A remains active',
    }
    (OUT / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')
    if command('systemctl', 'is-active', '--quiet', 'rkaiq_3A.service').returncode != 0:
        raise RuntimeError('3A is not active')
    if command('fuser', '/dev/video5', '/dev/video14').returncode == 0:
        raise RuntimeError('capture device busy')
    ports = command('ss', '-H', '-ltn', 'sport = :8554 or sport = :8555')
    if ports.returncode or ports.stdout.strip():
        raise RuntimeError('RTSP port check failed or port busy')
    before = command('dmesg').stdout
    (OUT / 'dmesg-before.txt').write_text(before)
    (OUT / 'started.txt').write_text(datetime.datetime.now().astimezone().isoformat() + '\n')
    child = None
    reason = 'duration reached'
    d_since = {'imx415': None, 'ov8858': None}
    last_frame = {'imx415': None, 'ov8858': None}
    last_advance = {'imx415': None, 'ov8858': None}
    start = time.monotonic()
    try:
        with (OUT / 'supervisor.log').open('w') as log, (OUT / 'samples.jsonl').open('w') as samples:
            child = subprocess.Popen(plan['command'], stdout=log, stderr=subprocess.STDOUT)
            while True:
                now = time.monotonic()
                age = now - start
                row = {'elapsed_s': round(age, 3), 'wall': datetime.datetime.now().astimezone().isoformat(), 'cameras': {}}
                for name, isp in (('imx415', 0), ('ov8858', 1)):
                    rawpid = read(RUN / name / 'producer.pid').strip()
                    proc = read('/proc/' + rawpid + '/stat') if rawpid.isdigit() else ''
                    # The executable name is parenthesized and can contain spaces.
                    state = proc.rsplit(')', 1)[-1].strip().split(' ')[0] if ')' in proc else None
                    wchan = read('/proc/' + rawpid + '/wchan').strip() if state else None
                    match = re.search(r'Output\s+rkisp_mainpath[^\n]*\(frame:(\d+)', read('/proc/rkisp-vir' + str(isp)))
                    frame = int(match.group(1)) if match else None
                    row['cameras'][name] = {'pid': rawpid or None, 'state': state, 'wchan': wchan, 'isp_frame': frame}
                    if state == 'D':
                        d_since[name] = d_since[name] if d_since[name] is not None else now
                        if now - d_since[name] >= 3:
                            reason = name + ' D state >=3 s'
                    else:
                        d_since[name] = None
                    if frame is not None and frame != last_frame[name]:
                        last_frame[name] = frame
                        last_advance[name] = now
                    if age > 12 and last_advance[name] is None:
                        reason = name + ' no observed frame by 12 s'
                    if last_advance[name] is not None and now - last_advance[name] >= 5:
                        reason = name + ' ISP frame did not advance for 5 s'
                samples.write(json.dumps(row, ensure_ascii=False) + '\n')
                samples.flush()
                if child.poll() is not None:
                    reason = 'supervisor exited early rc=' + str(child.returncode)
                    break
                if reason != 'duration reached' or age >= plan['duration_s']:
                    break
                time.sleep(plan['sample_interval_s'])
    finally:
        if child is not None and child.poll() is None:
            child.send_signal(signal.SIGTERM)
            try:
                child.wait(timeout=150)
            except subprocess.TimeoutExpired:
                reason += '; supervisor did not exit within 150 s; inspect board'
        after = command('dmesg').stdout
        (OUT / 'dmesg-after.txt').write_text(after)
        if after.startswith(before):
            (OUT / 'dmesg-delta.txt').write_text(after[len(before):])
        else:
            (OUT / 'dmesg-delta.txt').write_text('dmesg ring changed; compare full snapshots\n')
        status = {'reason': reason, 'supervisor_returncode': child.poll() if child else None,
                  '3a': command('systemctl', 'is-active', 'rkaiq_3A.service').stdout.strip(),
                  'device_users': command('fuser', '/dev/video5', '/dev/video14').stdout.strip(),
                  'ended': datetime.datetime.now().astimezone().isoformat()}
        (OUT / 'watch-status.json').write_text(json.dumps(status, indent=2) + '\n')
    return 0 if reason == 'duration reached' and child and child.returncode == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
