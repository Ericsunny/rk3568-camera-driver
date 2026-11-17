#!/usr/bin/env python3
"""Trace default versus explicit software decode of one saved frame; no capture."""
import json
from pathlib import Path
import subprocess

base = Path(__file__).resolve().parent
source = next((base / 'event-imx-dma-r1').glob('event*.h264'))
results = {}
for mode, extra in [('default', []), ('software', ['-hwaccel', 'none', '-c:v', 'h264'])]:
    before = subprocess.check_output(['dmesg']).decode('utf-8', 'replace').splitlines()
    command = ['strace', '-f', '-yy', '-e', 'trace=ioctl', '-o', str(base / (mode + '-decode.trace')),
               'ffmpeg', '-nostdin', '-v', 'info', *extra, '-i', str(source),
               '-frames:v', '1', '-an', '-f', 'null', '-']
    with (base / (mode + '-decode.log')).open('wb') as log:
        proc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    after = subprocess.check_output(['dmesg']).decode('utf-8', 'replace').splitlines()
    delta = after[len(before):] if after[:len(before)] == before else None
    results[mode] = {'argv': command, 'rc': proc.returncode, 'dmesg_delta': delta}
(base / 'decoder-probe.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results), flush=True)
