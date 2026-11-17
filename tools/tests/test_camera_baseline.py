#!/usr/bin/env python3
"""Reject false functional passes; these tests do not simulate camera hardware."""
import copy
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'validation' / 'camera_baseline.py'
spec = importlib.util.spec_from_file_location('camera_baseline', path)
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


class BaselineResultTest(unittest.TestCase):
    def setUp(self):
        self.result = dict(service_rc=0, triggered_with_two_clients=True,
                           clients=[dict(rc=0, frames=90, errors=[]) for _ in range(2)],
                           delivery_status='complete', delivery_frames=79,
                           software_decode=[dict(rc=0, stderr='', frames=79) for _ in range(2)],
                           after={'3a': {'rc': 0}, 'busy': {'rc': 1}},
                           format_restored=True, service_alive=False,
                           port_after={'rc': 0, 'stdout': ''})

    def test_control_and_decoded_frame_counter(self):
        self.assertEqual(baseline.assess(self.result), [])
        self.assertEqual(baseline.progress_frames('frame=1\nframe=90\nprogress=end\n'), 90)

    def test_decode_errors_override_zero_exit(self):
        self.result['clients'][0]['errors'] = ['corrupt decoded frame']
        self.assertTrue(baseline.assess(self.result))

    def test_each_required_observation(self):
        for key, value in [('triggered_with_two_clients', False), ('service_rc', 1),
                           ('format_restored', False), ('service_alive', True),
                           ('stop', 'kernel alert'), ('software_decode', []),
                           ('clients', []), ('delivery_status', 'failed')]:
            r = copy.deepcopy(self.result)
            r[key] = value
            self.assertTrue(baseline.assess(r), key)

    def test_requested_metrics_require_validated_evidence(self):
        self.result['frame_metrics_requested'] = True
        self.assertTrue(baseline.assess(self.result))

    def test_format_and_missing_frame_evidence(self):
        fmt = "Width/Height : 3840/2160\nPixel Format : 'NV12'"
        self.assertEqual(baseline.parse_format(fmt), {'width': 3840, 'height': 2160, 'fourcc': 'NV12'})
        with self.assertRaises(RuntimeError):
            baseline.parse_format('driver unavailable')
        self.assertEqual(baseline.progress_frames('progress=end\n'), 0)

    def test_software_counts_must_match(self):
        self.result['software_decode'][1]['frames'] = 78
        self.assertTrue(baseline.assess(self.result))

    def test_missing_software_count_is_not_a_pass(self):
        del self.result['software_decode'][0]['frames']
        self.assertTrue(baseline.assess(self.result))


if __name__ == '__main__':
    unittest.main()
