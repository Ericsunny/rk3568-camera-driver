#!/usr/bin/env python3
"""Exercise the C++ supervisor with isolated fake camera processes on Linux."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def wait_files(*paths):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        if all(p.exists() for p in paths):
            return
        time.sleep(.05)
    raise AssertionError('children did not start')


def main():
    with tempfile.TemporaryDirectory(prefix='dual-cpp-') as tmp:
        root = Path(tmp)
        binary = root / 'run_dual_camera'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2',
                        str(ROOT / 'tools/run_dual_camera.cpp'), '-o', str(binary)], check=True)
        child = root / 'child.py'
        child.write_text('''import os, pathlib, signal, sys, time
profile, mode, path = sys.argv[1:]
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
pathlib.Path(path).write_text(os.environ['RTSP_PORT_OVERRIDE'])
if os.environ.get('FAIL_ONE') and profile == 'imx415-isp':
    time.sleep(1)
    sys.exit(7)
while True: time.sleep(.05)
''')
        wrapper = root / 'wrapper.sh'
        wrapper.write_text('#!/bin/sh\nexec "' + sys.executable + '" "' + str(child) + '" "$@"\n')
        wrapper.chmod(0o755)

        def start(name, fail=False):
            env = dict(os.environ, CAMERA_DEMO_WRAPPER=str(wrapper), DUAL_SHUTDOWN_TIMEOUT='5')
            if fail:
                env['FAIL_ONE'] = '1'
            return subprocess.Popen([str(binary), 'manual', str(root / name)], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        run = root / 'normal'
        proc = start('normal')
        wait_files(run / 'imx415', run / 'ov8858')
        assert (run / 'imx415').read_text() == '8554'
        assert (run / 'ov8858').read_text() == '8555'
        proc.send_signal(signal.SIGTERM)
        out, err = proc.communicate(timeout=8)
        assert proc.returncode == 0, (out, err)
        data = json.loads((run / 'status.json').read_text())
        assert data['status'] == 'stopped'
        assert all(c['returncode'] == 0 for c in data['children'].values())
        original = (run / 'status.json').read_bytes()
        assert start('normal').wait(timeout=8) != 0
        assert (run / 'status.json').read_bytes() == original

        run = root / 'failure'
        proc = start('failure', fail=True)
        wait_files(run / 'imx415', run / 'ov8858')
        out, err = proc.communicate(timeout=8)
        assert proc.returncode == 1, (out, err)
        data = json.loads((run / 'status.json').read_text())
        assert data['status'] == 'failed'
        assert data['children']['imx415']['returncode'] == 7
        assert data['children']['ov8858']['returncode'] == 0
        bad = root / 'bad'
        result = subprocess.run([str(binary), 'manual', str(bad), '--imx-port', '8554',
                                 '--ov-port', '8554'], capture_output=True)
        assert result.returncode != 0 and not bad.exists()
        print('PASS C++ dual service: ports, clean stop, peer failure, no overwrite')


if __name__ == '__main__':
    main()
