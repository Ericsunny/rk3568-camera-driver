#!/usr/bin/env python3
"""Compare software-decoded H.264 and MP4 frames; does not access cameras."""
import json
from pathlib import Path
import subprocess
import sys

d = Path(sys.argv[1])
sources = list(d.glob('event*.h264'))
if len(sources) != 1:
    raise SystemExit('expected one H.264 event')
source = sources[0]
mp4 = source.with_suffix('.mp4')
result = {}
for name, path in [('source', source), ('mp4', mp4)]:
    output = d / (name + '-software.framemd5')
    cmd = ['ffmpeg', '-nostdin', '-v', 'error', '-hwaccel', 'none', '-c:v', 'h264',
           '-i', str(path), '-map', '0:v:0', '-vsync', '0', '-f', 'framemd5', str(output)]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    (d / (name + '-software.stderr')).write_bytes(p.stderr)
    rows = output.read_text().splitlines() if output.exists() else []
    hashes = [r.rsplit(',', 1)[-1].strip() for r in rows if r and not r.startswith('#')]
    result[name] = {'argv': cmd, 'rc': p.returncode, 'stderr_bytes': len(p.stderr), 'hashes': hashes}
result['same_frames'] = bool(result['source']['hashes']) and result['source']['hashes'] == result['mp4']['hashes']
result['frames'] = len(result['source']['hashes'])
result['pass'] = result['same_frames'] and all(result[k]['rc'] == 0 and result[k]['stderr_bytes'] == 0
                                              for k in ('source', 'mp4'))
(d / 'software-verify.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({'directory': str(d), 'frames': result['frames'], 'pass': result['pass']}))
sys.exit(0 if result['pass'] else 1)
