#!/usr/bin/env python3
"""Read-only version-command trace; does not open a camera or decode footage."""
import json
from pathlib import Path
import subprocess

base = Path(__file__).resolve().parent
before = subprocess.check_output(['dmesg']).decode('utf-8', 'replace').splitlines()
command = ['strace', '-f', '-yy', '-e', 'trace=ioctl', '-o', str(base / 'ffmpeg-version.trace'),
           'ffmpeg', '-version']
with (base / 'ffmpeg-version.log').open('wb') as log:
    p = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=15)
after = subprocess.check_output(['dmesg']).decode('utf-8', 'replace').splitlines()
delta = after[len(before):] if after[:len(before)] == before else None
result = {'argv': command, 'rc': p.returncode, 'dmesg_delta': delta,
          'version_probe_messages': sum('rga2: unknown ioctl cmd!' in line for line in delta) if delta is not None else None}
(base / 'ffmpeg-init-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
