#!/usr/bin/env python3
"""Exercise the recorder with synthetic child processes, never a camera."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RECORDER = ROOT / 'tools/window_recorder.py'
SERVICE = '''import signal, sys, time
def term(s, f): sys.exit(0)
def trigger(s, f): print("trigger", flush=True)
signal.signal(signal.SIGTERM, term)
signal.signal(signal.SIGUSR1, trigger)
print("ready", flush=True)
while True: time.sleep(.05)
'''


class RecorderTest(unittest.TestCase):
    def setUp(self):
        # The recorder rejects /tmp; choose an isolated directory in the current workspace.
        self.tmp = tempfile.TemporaryDirectory(prefix='window-rec-test-', dir=os.getcwd())
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'; self.bin.mkdir()
        self.temp = self.root / 'temperature'; self.temp.write_text('35000')
        self.service = self.root / 'service.py'; self.service.write_text(SERVICE)
        self.dmesg = self.bin / 'dmesg'
        self.dmesg.write_text('#!/bin/sh\nexec sleep 30\n'); self.dmesg.chmod(0o755)
        self.env = os.environ.copy()
        self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']
        self.args = ['--parent', str(self.root), '--duration', '4', '--interval', '1',
                     '--trigger-every', '2', '--first-trigger', '1',
                     '--reserve-bytes', '1', '--reserve-inodes', '1', '--budget-bytes', '1',
                     '--temperature', str(self.temp), '--max-temp', '80000', '--',
                     sys.executable, '-u', str(self.service), '{run}']

    def tearDown(self):
        self.tmp.cleanup()

    def execute(self):
        return subprocess.run([sys.executable, str(RECORDER)] + self.args, env=self.env,
                              text=True, capture_output=True, timeout=20)

    def test_autonomous_run_and_triggers(self):
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        run = Path(result.stdout.strip())
        summary = json.loads((run / 'summary.json').read_text())
        self.assertEqual(summary['verdict'], 'NOT_EVALUATED')
        self.assertEqual(summary['reason'], 'duration elapsed')
        self.assertEqual(summary['sample_deadlines_missed'], 0)
        self.assertEqual(summary['triggers_sent'], 2)
        self.assertEqual((run / 'service.log').read_text().count('trigger'), 2)
        with (run / 'samples.csv').open() as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), 4)
        self.assertEqual(len({r['boot_id'] for r in rows}), 1)
        self.assertEqual(len({(r['pid'], r['start_ticks']) for r in rows}), 1)
        self.assertEqual(len((run / 'triggers.jsonl').read_text().splitlines()), 2)

    def test_invalid_temperature_prevents_start(self):
        self.temp.write_text('80000')
        result = self.execute()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('temperature', result.stderr)
        self.assertEqual(list(self.root.glob('window-*')), [])

    def test_kernel_follower_failure_stops_before_service(self):
        self.dmesg.write_text('#!/bin/sh\nexit 1\n')
        result = self.execute()
        self.assertNotEqual(result.returncode, 0)
        run = Path(result.stdout.strip())
        summary = json.loads((run / 'summary.json').read_text())
        self.assertIn('kernel log follower failed', summary['reason'])
        self.assertIsNone(summary['service_returncode'])

    def test_insufficient_storage_prevents_start(self):
        self.args[self.args.index('--budget-bytes') + 1] = str(2**62)
        result = self.execute()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('capacity', result.stderr)
        self.assertEqual(list(self.root.glob('window-*')), [])


if __name__ == '__main__':
    unittest.main()
