#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import time

base = Path(__file__).resolve().parent
commands = {
    'time': ['date', '+%F %T %z'],
    'uname': ['uname', '-a'],
    '3a': ['systemctl', 'is-active', 'rkaiq_3A'],
    'driver': ['readlink', '/sys/bus/i2c/devices/5-0036/driver'],
    'format_imx': ['v4l2-ctl', '-d', '/dev/video5', '--get-fmt-video'],
    'format_ov': ['v4l2-ctl', '-d', '/dev/video14', '--get-fmt-video'],
    'fuser': ['fuser', '/dev/video5', '/dev/video14'],
    'boot_sha': ['sha256sum', '/boot/Image-4.19.232', '/boot/rk-kernel.dtb'],
    'candidate_sha': ['sha256sum', str(base / 'candidate-v3/evrec'), str(base / 'candidate-v3/tools/evrec.c')],
    'ports': ['ss', '-H', '-ltn', 'sport = :18754 or sport = :18755 or sport = :18760'],
}
result = {'utc': time.strftime('%FT%TZ', time.gmtime())}
for name, cmd in commands.items():
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    result[name] = {'argv': cmd, 'rc': proc.returncode, 'output': proc.stdout.decode('utf-8', 'replace')}
(base / 'final-state.json').write_text(json.dumps(result, indent=2) + '\n')
ok = (result['3a']['rc'] == 0 and result['fuser']['rc'] == 1 and not result['ports']['output'].strip()
      and 'ov8858-sm' in result['driver']['output'])
print(json.dumps({'pass': ok, '3a': result['3a']['output'].strip(),
                  'capture_nodes_busy': result['fuser']['rc'] == 0, 'ports': result['ports']['output'].strip()}))
