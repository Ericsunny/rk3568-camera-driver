#!/usr/bin/env python3
"""Frame association, clock, ownership-order and incomplete evidence checks."""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

module = Path(__file__).resolve().parents[1] / 'analyze_frame_metrics.py'
spec = importlib.util.spec_from_file_location('frame_metrics', module)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class MetricsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'frames.csv'
        self.rows = []
        for i in range(4):
            dq = (i + 1) * 100000000
            self.rows.append(dict(zip(m.COLUMNS, [1, i, i, 0, 0x2000, dq - 1000000,
                dq, dq + 2000000, dq + 3000000, dq + 4000000, 1, 1])))

    def tearDown(self):
        self.tmp.cleanup()

    def run_case(self, **override):
        with self.path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, m.COLUMNS)
            writer.writeheader()
            writer.writerows(self.rows)
        meta = dict(schema=1, clock='CLOCK_MONOTONIC', capacity=4096, seen=len(self.rows),
                    stored=len(self.rows), completed_total=sum(r['completed'] for r in self.rows),
                    epochs=max(r['epoch'] for r in self.rows), truncated=False, write_error=False)
        meta.update(override)
        Path(str(self.path) + '.meta').write_text(json.dumps(meta))
        return m.analyze(self.path, 0)

    def test_known_timings(self):
        r = self.run_case()
        self.assertAlmostEqual(r['observed_dq_fps'], 10)
        self.assertEqual(r['timings']['dq_to_encode_done']['median_ms'], 2)
        self.assertEqual(r['timings']['driver_timestamp_to_dq']['p95_ms'], 1)

    def test_wrap_and_new_epoch(self):
        for row, seq in zip(self.rows, (0xFFFFFFFE, 0xFFFFFFFF, 0, 2)):
            row['driver_sequence'] = seq
        self.assertEqual(self.run_case()['driver_sequence_gaps'], 1)
        self.rows[2]['epoch'] = self.rows[3]['epoch'] = 2
        self.rows[2]['driver_sequence'], self.rows[3]['driver_sequence'] = 100, 101
        self.assertEqual(self.run_case()['driver_sequence_gaps'], 0)

    def test_invalid_clocks_do_not_create_frame_age(self):
        for row in self.rows:
            row['buffer_flags'] = 0
        r = self.run_case()
        self.assertIsNone(r['timings']['driver_timestamp_to_encode_done'])
        self.assertEqual(r['driver_age_unavailable'], 4)

    def test_invalid_order_or_backward_sequence_rejected(self):
        self.rows[2]['input_return_mono_ns'] = 1
        with self.assertRaises(ValueError): self.run_case()
        self.rows[2]['input_return_mono_ns'] = self.rows[2]['dq_mono_ns'] + 3000000
        self.rows[2]['driver_sequence'] = 0
        with self.assertRaises(ValueError): self.run_case()

    def test_middle_clock_reversal_is_rejected(self):
        self.rows[2]['dq_mono_ns'] = self.rows[1]['dq_mono_ns']
        with self.assertRaises(ValueError): self.run_case()

    def test_copy_order_and_incomplete_row(self):
        for row in self.rows:
            row.update(input_dmabuf=0, input_return_mono_ns=0,
                       qbuf_done_mono_ns=row['dq_mono_ns'] + 1000000)
        self.rows[-1].update(completed=0, encode_done_mono_ns=0)
        self.assertEqual(self.run_case()['incomplete'], 1)

    def test_truncation_and_write_failure_rejected(self):
        for changed in ({'seen': 4097, 'truncated': True}, {'write_error': True}, {'stored': 1}):
            with self.assertRaises(ValueError): self.run_case(**changed)


if __name__ == '__main__':
    unittest.main()
