#!/usr/bin/env python3
"""Host tests for paired service lifecycle; no board or camera required."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def wait_for_files(*paths):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if all(p.exists() for p in paths):
            return
        time.sleep(.05)
    raise AssertionError('children did not start')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        child = root / 'child.py'
        child.write_text('''import os, pathlib, signal, sys, time
profile, mode, path = sys.argv[1:]
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
pathlib.Path(path).write_text(os.environ['RTSP_PORT_OVERRIDE'])
if os.environ.get('FAIL_ONE') and profile == 'imx415-isp':
    time.sleep(1)
    sys.exit(7)
while True:
    time.sleep(.05)
''')
        wrapper = root / 'wrapper.sh'
        wrapper.write_text('exec "' + sys.executable + '" "' + str(child) + '" "$@"\n')
        script = ("import sys; sys.path.insert(0, sys.argv[1]); from run_dual_camera import run_pair; "
                  "from pathlib import Path; sys.exit(run_pair('manual', Path(sys.argv[2]), "
                  "Path(sys.argv[3]), 8554, 8555, shutdown_timeout=5))")

        def start(name, fail=False):
            return subprocess.Popen([sys.executable, '-c', script, str(ROOT / 'tools'),
                                     str(root / name), str(wrapper)],
                                    env=dict(os.environ, FAIL_ONE='1' if fail else ''),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        run = root / 'normal'
        process = start('normal')
        wait_for_files(run / 'imx415', run / 'ov8858')
        assert (run / 'imx415').read_text() == '8554'
        assert (run / 'ov8858').read_text() == '8555'
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=8) == 0
        result = json.loads((run / 'status.json').read_text())
        assert result['status'] == 'stopped'
        assert all(c['returncode'] == 0 for c in result['children'].values())
        original = (run / 'status.json').read_bytes()
        assert start('normal').wait(timeout=8) != 0
        assert (run / 'status.json').read_bytes() == original

        run = root / 'failure'
        process = start('failure', fail=True)
        wait_for_files(run / 'imx415', run / 'ov8858')
        assert process.wait(timeout=8) == 1
        result = json.loads((run / 'status.json').read_text())
        assert result['status'] == 'failed'
        assert result['children']['imx415']['returncode'] == 7
        assert result['children']['ov8858']['returncode'] == 0

        cmd = [sys.executable, str(ROOT / 'tools/run_dual_camera.py'), 'manual',
               str(root / 'bad'), '--imx-port', '8554', '--ov-port', '8554']
        assert subprocess.run(cmd, capture_output=True).returncode != 0
        assert not (root / 'bad').exists()
        print('PASS dual camera: isolated ports, clean stop, peer failure propagation')


if __name__ == '__main__':
    main()
