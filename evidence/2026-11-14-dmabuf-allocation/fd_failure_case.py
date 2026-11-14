#!/usr/bin/env python3
"""Inject one invalid shared-input fd in the isolated recorder."""
import json
import os
from pathlib import Path
import subprocess
import sys

base = Path(__file__).resolve().parent
out = base / 'final-invalid-fd'
env = dict(os.environ, LD_PRELOAD=str(base / 'libfail_input_fd.so'))
command = ['python3', str(base / 'run_recorder.py'), str(base / 'candidate-v3/evrec'),
           '/dev/video5', '1920', '1080', '15', 'dmabuf', str(out)]
proc = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
(base / 'fd-failure-controller.log').write_bytes(proc.stdout)
result = json.loads((out / 'result.json').read_text())
log = (out / 'run.log').read_text(errors='replace')
passed = (proc.returncode != 0 and result['rc'] != 0 and not result['stop'] and
          not result['child_alive'] and result['busy_after']['rc'] == 1 and
          result['3a_after']['rc'] == 0 and
          log.count('[fault] shared main-loop fd lookup returned -1') == 1 and
          '共享输入 DMA-BUF fd 无效' in log and
          'mode=dmabuf copy_bytes=0 returned=0' in log and not list(out.glob('event*.h264')))
summary = {'controller_rc': proc.returncode, 'recorder_rc': result['rc'],
           'injection_hit': '[fault] shared main-loop fd lookup returned -1' in log,
           'no_silent_fallback': 'mode=dmabuf copy_bytes=0 returned=0' in log,
           'pass': bool(passed)}
(base / 'fd-failure-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary), flush=True)
sys.exit(0 if passed else 1)
