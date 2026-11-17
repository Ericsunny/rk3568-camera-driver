#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('directory')
p.add_argument('frames', type=int)
a = p.parse_args()
d = Path(a.directory)
result = {}
for mode in ('copy', 'dmabuf'):
    source = d / (mode + '.h264')
    output = d / (mode + '.framemd5')
    cmd = ['ffmpeg', '-nostdin', '-v', 'error', '-i', str(source), '-map', '0:v:0',
           '-vsync', '0', '-f', 'framemd5', str(output)]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    (d / (mode + '.decode.stderr')).write_bytes(proc.stderr)
    lines = output.read_text().splitlines() if output.exists() else []
    hashes = [line.rsplit(',', 1)[-1].strip() for line in lines if line and not line.startswith('#')]
    result[mode] = {'argv': cmd, 'rc': proc.returncode, 'stderr_bytes': len(proc.stderr),
                    'frames': len(hashes), 'hashes': hashes,
                    'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
result['same_frames'] = (len(result['copy']['hashes']) == a.frames and
                          result['copy']['hashes'] == result['dmabuf']['hashes'])
result['differing_frames'] = sum(x != y for x, y in zip(result['copy']['hashes'], result['dmabuf']['hashes']))
result['first_different'] = next((n for n, (x, y) in enumerate(zip(result['copy']['hashes'], result['dmabuf']['hashes'])) if x != y), None)
(d / 'compare.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({k: {x: y for x, y in v.items() if x != 'hashes'} if isinstance(v, dict) else v for k, v in result.items()}))
raise SystemExit(0 if result['same_frames'] and all(result[m]['rc'] == 0 and not result[m]['stderr_bytes'] for m in ('copy', 'dmabuf')) else 1)
