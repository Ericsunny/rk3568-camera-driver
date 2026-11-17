#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time

p = argparse.ArgumentParser()
p.add_argument('--binary', required=True)
p.add_argument('--device', required=True)
p.add_argument('--out', required=True)
p.add_argument('--width', type=int, default=1920)
p.add_argument('--height', type=int, default=1080)
a = p.parse_args()
out = Path(a.out)
out.mkdir(mode=0o700, parents=False, exist_ok=False)
cmd = [a.binary, '-d', a.device, '-w', str(a.width), '-h', str(a.height),
       '--fps', '15', '--kbps', '4000', '--gop', '15', '--pre', '3',
       '--post', '2', '--ring-mb', '64', '--max-events', '1', '--recover', '0',
       '--no-osd', '-o', str(out / 'event')]
result = {'argv': cmd, 'trigger_after_s': 6, 'timed_out': False}
t0 = time.monotonic()
result['start_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
with (out / 'run.log').open('wb') as f:
    proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
    time.sleep(6)
    if proc.poll() is None:
        proc.send_signal(signal.SIGUSR1)
        result['trigger_sent'] = True
    else:
        result['trigger_sent'] = False
    try:
        result['exit_code'] = proc.wait(timeout=24)
    except subprocess.TimeoutExpired:
        result['timed_out'] = True
        proc.send_signal(signal.SIGTERM)
        try:
            result['exit_code'] = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                result['exit_code'] = proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                result['exit_code'] = None
                result['remaining_pid'] = proc.pid
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
result.update(elapsed_s=round(time.monotonic() - t0, 4), user_s=usage.ru_utime,
              system_s=usage.ru_stime, maxrss_kb=usage.ru_maxrss,
              end_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
(out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, ensure_ascii=False))
sys.exit(0 if result.get('exit_code') == 0 and not result['timed_out'] and result['trigger_sent'] else 1)
