#!/usr/bin/env python3
"""Compare C++ graph selection with the Python reference on synthetic topologies."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    fixtures = runpy.run_path(str(ROOT / 'tools/tests/test_resolve_camera.py'))
    with tempfile.TemporaryDirectory(prefix='resolve-cpp-') as tmp:
        root = Path(tmp)
        binary = root / 'resolve_camera'
        subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'tools/resolve_camera.cpp'), '-o', str(binary)], check=True)
        tool = root / 'media-ctl'
        tool.write_text('#!/bin/sh\ncat "$MEDIA_FIXTURE_DIR/$(basename "$2")"\n')
        tool.chmod(0o755)
        env = dict(os.environ, MEDIA_FIXTURE_DIR=str(root), PATH=str(root) + ':' + os.environ['PATH'])

        def run(profile, inputs, expected=None):
            paths = []
            for name, contents in inputs:
                (root / name).write_text(contents)
                paths.append('/dev/' + name)
            p = subprocess.run([str(binary), profile, *paths], env=env, text=True, capture_output=True)
            if expected is None:
                assert p.returncode != 0, (profile, p.stdout, p.stderr)
            else:
                assert p.returncode == 0 and p.stdout.strip() == expected, (p.stdout, p.stderr)

        graph, cif, isp = (fixtures[x] for x in ('graph', 'cif', 'isp'))
        run('imx415-isp', [('media1', graph)], '/dev/video5')
        run('imx415-isp', [('media1', graph.replace('driver          rkisp-vir0',
                                                     'driver          rkisp-vir0\ndriver version 4.19.255'))], '/dev/video5')
        run('ov8858-isp', [('media0', cif), ('media2', isp)], '/dev/video14')
        run('imx415-isp', [('media1', graph.replace('[ENABLED]', '[]', 1))])
        run('imx415-isp', [('media1', graph), ('media2', graph)])
        run('ov8858-isp', [('media1', graph)])
        run('ov8858-isp', [('media0', cif.replace('[ENABLED]', '[]', 1)), ('media2', isp)])
        run('ov8858-isp', [('media9', '')])
        print('PASS C++ graph selection, disconnected/ambiguous rejection')


if __name__ == '__main__':
    main()
