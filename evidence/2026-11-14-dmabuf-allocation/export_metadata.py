#!/usr/bin/env python3
"""Archive only bounded text, JSON, source and frame hashes, never media."""
from pathlib import Path
import hashlib
import json
import tarfile

base = Path(__file__).resolve().parent
output = base / 'metadata-only.tar.gz'
allowed = {'.log', '.json', '.txt', '.trace', '.framemd5', '.c', '.h', '.py', '.sh', '.csv', '.stderr', '.env'}
files = []
for path in sorted(base.rglob('*')):
    if not path.is_file() or path.is_symlink() or path.suffix not in allowed:
        continue
    if path.stat().st_size > 5 * 1024 * 1024:
        raise SystemExit('unexpected large text artifact: ' + str(path))
    files.append(path)
with tarfile.open(output, 'x:gz') as archive:
    for path in files:
        archive.add(path, arcname=str(path.relative_to(base)), recursive=False)
digest = hashlib.sha256(output.read_bytes()).hexdigest()
print(json.dumps({'path': str(output), 'entries': len(files), 'sha256': digest,
                  'allowed_suffixes': sorted(allowed)}, indent=2))
