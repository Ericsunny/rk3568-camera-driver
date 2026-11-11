#!/usr/bin/env python3
"""Four synthetic writers exercise orchestration, not RTSP or decoding."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/rtsp_four_record.py'
FAKE = '''#!%s
import signal, sys, time
def stop(s, f): sys.exit(0)
signal.signal(signal.SIGINT, stop)
with open(sys.argv[-1], "xb", buffering=0) as output:
    while True:
        output.write(b"test packet")
        time.sleep(.1)
''' % sys.executable


class FourTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rtsp4-test-', dir=os.getcwd())
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'; self.bin.mkdir()
        fake = self.bin / 'ffmpeg'; fake.write_text(FAKE); fake.chmod(0o755)
        self.temp = self.root / 'temperature'; self.temp.write_text('35000')
        self.env = os.environ.copy(); self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']
        self.args = ['--parent', str(self.root), '--url', 'rtsp://127.0.0.1/live',
                     '--duration', '3', '--sample-interval', '1', '--startup-timeout', '5',
                     '--budget-bytes', '1', '--reserve-bytes', '1', '--reserve-inodes', '1',
                     '--temperature', str(self.temp), '--max-temp', '80000']

    def tearDown(self): self.tmp.cleanup()

    def execute(self):
        return subprocess.run([sys.executable, str(TOOL)] + self.args, env=self.env,
                              text=True, capture_output=True, timeout=20)

    def test_four_processes_and_captured_outputs(self):
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        run = Path(result.stdout.strip())
        summary = json.loads((run / 'summary.json').read_text())
        self.assertEqual(summary['reason'], 'duration elapsed')
        self.assertEqual(summary['verdict'], 'NOT_EVALUATED')
        self.assertEqual(summary['client_returncodes'], [0] * 4)
        self.assertTrue(all((run / ('client%d.ts' % i)).stat().st_size > 0 for i in range(1, 5)))
        samples = [json.loads(line) for line in (run / 'samples.jsonl').read_text().splitlines()]
        self.assertEqual(len(set(samples[-1]['pids'])), 4)

    def test_reject_credentials(self):
        self.args[self.args.index('--url') + 1] = 'rtsp://user:pass@127.0.0.1/live'
        result = self.execute()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.root.glob('rtsp4-*')), [])

    def test_storage_preflight(self):
        self.args[self.args.index('--budget-bytes') + 1] = str(2**62)
        result = self.execute()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('capacity', result.stderr)


if __name__ == '__main__': unittest.main()
