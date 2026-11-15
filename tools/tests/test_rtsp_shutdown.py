#!/usr/bin/env python3
"""Linux host socketpair test; does not access a camera or board."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--baseline', action='store_true', help='reproduce with committed hub source')
p.add_argument('--baseline-source', type=Path, help='reproduce with a saved hub source')
args = p.parse_args()
source = (args.baseline_source.read_bytes() if args.baseline_source else
          subprocess.check_output(['git', 'show', 'HEAD:tools/rtsp_hub.c'], cwd=root)
          if args.baseline else (root / 'tools/rtsp_hub.c').read_bytes())
with tempfile.TemporaryDirectory(prefix='rtsp-shutdown-') as tmp:
    path = Path(tmp)
    (path / 'tests').mkdir()
    (path / 'rtsp_hub.c').write_bytes(source)
    (path / 'rtsp_hub.h').write_bytes((root / 'tools/rtsp_hub.h').read_bytes())
    (path / 'tests/test.c').write_bytes((root / 'tools/tests/rtsp_shutdown_test.c').read_bytes())
    binary = path / 'test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(path / 'tests/test.c'), '-o', str(binary)], check=True)
    run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         timeout=5)
    print(run.stdout.decode('utf-8', 'replace'), end='')
    if args.baseline or args.baseline_source:
        assert run.returncode != 0 and b'FAIL' in run.stdout, 'baseline did not reproduce'
        print('committed baseline interrupts a fragmented NAL: reproduced')
    else:
        assert run.returncode == 0, 'shutdown test failed'
