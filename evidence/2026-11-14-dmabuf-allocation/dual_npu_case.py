#!/usr/bin/env python3
import json
import os
from pathlib import Path
import signal
import subprocess
import time

base = Path(__file__).resolve().parent
jobs, logs = [], []
try:
    for name, dev, w, h, fps, port in [('imx', '/dev/video5', 1920, 1080, 15, 18754),
                                       ('ov', '/dev/video14', 1280, 960, 30, 18755)]:
        out = base / ('dual-npu-' + name)
        cmd = ['python3', str(base / 'rtsp_case.py'), dev, str(w), str(h), str(fps), str(out), '--port', str(port), '--detect', '--dual-detect', '--binary', str(base / ('dual_npu_' + name + '.sh'))]
        log = (base / ('dual-npu-' + name + '-controller.log')).open('wb')
        logs.append(log)
        jobs.append((out, subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)))
    until = time.monotonic() + 60
    while any(proc.poll() is None for _, proc in jobs):
        if any(proc.poll() not in (None, 0) for _, proc in jobs):
            raise RuntimeError('one channel failed')
        if time.monotonic() >= until:
            raise RuntimeError('joint test timeout')
        time.sleep(.1)
    results = {out.name: proc.returncode for out, proc in jobs}
    (base / 'dual-npu-result.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results), flush=True)
finally:
    for out, proc in jobs:
        if proc.poll() is None:
            pid_file = out / 'producer.pid'
            if pid_file.exists():
                try:
                    os.kill(int(pid_file.read_text()), signal.SIGTERM)
                except ProcessLookupError:
                    pass
            proc.wait(timeout=20)
    for log in logs:
        log.close()
