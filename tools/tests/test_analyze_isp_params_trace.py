#!/usr/bin/env python3
"""Synthetic records exercise the trace analyzer; no camera required."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_isp_params_trace import analyze


class TraceTest(unittest.TestCase):
    def test_only_mapped_accepted_records_count_flags(self):
        rows = [
            'MMAP dev=22 fd=10 off=0 len=10475\n',
            't=1.0 dev=22 idx=0 mapped=1 frame=0 en_update=0000020000000000 ens=0000020000000000 cfg_update=0000020000000000 rc=0 errno=0\n',
            't=2.0 dev=22 idx=1 mapped=1 frame=1 en_update=0000000000000000 ens=0000020000000000 cfg_update=0000000000000000 rc=-1 errno=5\n',
            't=3.0 dev=22 idx=2 mapped=0 frame=0 en_update=0000000000000000 ens=0000000000000000 cfg_update=0000000000000000 rc=0 errno=0\n',
            'MAP_OVERFLOW\n',
        ]
        result = analyze(rows)
        d = result['devices']['22']
        self.assertEqual((d['attempts'], d['mapped'], d['accepted']), (3, 2, 1))
        self.assertEqual((d['drc_enable_update'], d['drc_enabled_bit'], d['drc_config_update']), (1, 1, 1))
        self.assertEqual((d['first_frame'], d['last_frame']), (0, 0))
        self.assertEqual(result['overflow_events'], 1)

    def test_distinguishes_merge_and_devices(self):
        line = 't=1 dev={} idx=0 mapped=1 frame=3 en_update=0000000008000000 ens=0000000008000000 cfg_update=0000000008000000 rc=0 errno=0\n'
        result = analyze([line.format('13'), line.format('22')])
        for d in result['devices'].values():
            self.assertEqual(d['merge_config_update'], 1)
            self.assertEqual(d['drc_config_update'], 0)


if __name__ == '__main__':
    unittest.main()
