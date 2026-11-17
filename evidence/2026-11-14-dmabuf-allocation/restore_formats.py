#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess

base = Path(__file__).resolve().parent
results = {}
if subprocess.run(['systemctl', 'is-active', '--quiet', 'rkaiq_3A']).returncode:
    raise SystemExit('3A is not active; do not restore formats')
for device, width, height, initial in [('/dev/video5', 3840, 2160, 'imx-import-r1'),
                                        ('/dev/video14', 1632, 1224, 'ov-import-r1')]:
    busy = subprocess.run(['fuser', device], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if busy.returncode != 1:
        raise SystemExit(device + ' occupied or fuser failed')
    expected = json.loads((base / initial / 'precheck.json').read_text())['format']['output']
    command = ['v4l2-ctl', '-d', device,
               '--set-fmt-video=width=%d,height=%d,pixelformat=NV12' % (width, height)]
    set_result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    get = subprocess.run(['v4l2-ctl', '-d', device, '--get-fmt-video'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    actual = get.stdout.decode('utf-8', 'replace')
    results[device] = {'set_argv': command, 'set_rc': set_result.returncode,
                       'get_rc': get.returncode, 'equals_initial': actual == expected,
                       'before': expected, 'after': actual,
                       'stderr': (set_result.stderr + get.stderr).decode('utf-8', 'replace')}
    if set_result.returncode or get.returncode or actual != expected:
        (base / 'restore-formats.json').write_text(json.dumps(results, indent=2) + '\n')
        raise SystemExit(device + ' restore failed')
results['3a_after'] = subprocess.run(['systemctl', 'is-active', 'rkaiq_3A'],
                                       stdout=subprocess.PIPE).stdout.decode().strip()
(base / 'restore-formats.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps({dev: item if isinstance(item, str) else item['equals_initial']
                  for dev, item in results.items()}))
