#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('directory')
p.add_argument('service')
p.add_argument('frame_bytes', type=int)
a = p.parse_args()
d = Path(a.directory)
records = [f for f in d.glob('event*.json') if not f.name.endswith('.delivery.json')]
if len(records) != 1:
    raise SystemExit('expected exactly one closed event')
metadata = records[0]
cmd = ['python3', a.service, '--finalize-one', str(metadata)]
with (d / 'delivery.log').open('wb') as log:
    proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=120)
delivery_path = metadata.with_suffix('.delivery.json')
delivery = json.loads(delivery_path.read_text()) if delivery_path.exists() else {}
event = json.loads(metadata.read_text())
run = json.loads((d / 'result.json').read_text())
stats = re.search(r'mode=(copy|dmabuf) copy_bytes=(\d+) returned=(\d+)',
                  (d / 'run.log').read_text(errors='replace'))
processed = (int(stats[2]) // a.frame_bytes if stats[1] == 'copy' else int(stats[3])) if stats else 0
result = {'argv': cmd, 'remux_rc': proc.returncode, 'delivery': delivery,
          'write_error': event.get('write_error'), 'processed_frames': processed,
          'copy_bytes': int(stats[2]) if stats else None,
          'cpu_ms_per_processed_frame': ((run['user_s'] + run['system_s']) * 1000 / processed) if processed else None,
          'timing_scope': 'whole recorder child process CPU, including startup; no remux or RTSP client CPU'}
passed = (run['rc'] == 0 and run.get('triggered') and not run['stop'] and
          not run['child_alive'] and proc.returncode == 0 and
          delivery.get('status') == 'complete' and event.get('write_error') is False and
          processed > 0 and not list(d.glob('*.part')) and not list(d.glob('*.pending.*')))
result['pass'] = bool(passed)
(d / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({k: v for k, v in result.items() if k not in ('argv', 'delivery')}))
raise SystemExit(0 if passed else 1)
