#!/usr/bin/env python3
"""Check DRC IQ generation with synthetic JSON, without camera hardware."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from create_drc_iq_variant import create


class DrcVariantTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='drc-iq-test-')
        self.dir = Path(self.tmp.name)
        self.source = self.dir / 'source.json'
        self.output = self.dir / 'candidate.json'
        self.data = {'sensor_calib': {'CISFlip': 1}, 'main_scene': [{'name': 'normal',
            'sub_scene': [{'name': 'day', 'scene_isp21': {'adrc_calib': {'DrcTuningPara': {
                'Enable': 0, 'DrcGain': {'DrcGain': [1, 1, 1], 'DrcGain_len': 3},
                'Other': {'unchanged': 17}}}}}]}]}
        self.write_source()

    def tearDown(self):
        self.tmp.cleanup()

    def write_source(self):
        self.source.write_text(json.dumps(self.data), encoding='utf8')

    def drc(self, data):
        return data['main_scene'][0]['sub_scene'][0]['scene_isp21']['adrc_calib']['DrcTuningPara']

    def test_enable_only_preserves_source_and_other_fields(self):
        before = self.source.read_bytes()
        create(self.source, self.output)
        after = json.loads(self.output.read_text(encoding='utf8'))
        self.assertEqual(self.drc(after)['Enable'], 1)
        self.drc(after)['Enable'] = 0
        self.assertEqual(after, self.data)
        self.assertEqual(self.source.read_bytes(), before)

    def test_gain_override_and_no_overwrite(self):
        create(self.source, self.output, 2.0)
        data = json.loads(self.output.read_text(encoding='utf8'))
        self.assertEqual(self.drc(data)['DrcGain']['DrcGain'], [2.0] * 3)
        before = self.output.read_bytes()
        with self.assertRaises(FileExistsError):
            create(self.source, self.output, 3.0)
        self.assertEqual(self.output.read_bytes(), before)

    def test_invalid_parameters_do_not_create_output(self):
        for bad in (0.5, 9, float('nan'), float('inf')):
            with self.assertRaises(ValueError):
                create(self.source, self.output, bad)
            self.assertFalse(self.output.exists())
        self.drc(self.data)['DrcGain']['DrcGain_len'] = 2
        self.write_source()
        with self.assertRaises(ValueError):
            create(self.source, self.output, 2)
        self.assertFalse(self.output.exists())

    def test_rejects_ambiguous_scene(self):
        self.data['main_scene'].append(copy.deepcopy(self.data['main_scene'][0]))
        self.write_source()
        with self.assertRaises(ValueError):
            create(self.source, self.output)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
