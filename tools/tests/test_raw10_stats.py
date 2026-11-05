#!/usr/bin/env python3
"""Synthetic RAW10 fixtures only: no camera and no historical evidence input."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/raw10_stats.py'


def pack_raw10p(values):
    out = bytearray()
    for i in range(0, len(values), 4):
        block = values[i:i + 4]
        out.extend(v >> 2 for v in block)
        out.append(sum((v & 3) << (2 * k) for k, v in enumerate(block)))
    return bytes(out)


class RawStatsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='raw10-test-')
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.frames = []
        for index, values in enumerate((
                [4, 9, 14, 19, 24, 29, 34, 39],
                [8, 13, 18, 23, 28, 33, 38, 43])):
            path = self.root / f'frame{index}.raw'
            path.write_bytes(pack_raw10p(values[:4]) + b'\xaa\xbb' +
                             pack_raw10p(values[4:]) + b'\xcc\xdd')
            self.frames.append(path)

    def invoke(self, *extra, frames=None, packing='raw10p', stride='7'):
        command = [sys.executable, str(TOOL), '--packing', packing, '--bayer', 'BGGR',
                   '--width', '4', '--height', '2', '--stride', stride,
                   *extra, *(str(p) for p in (self.frames if frames is None else frames))]
        return subprocess.run(command, text=True, capture_output=True)

    def test_known_byte_order(self):
        self.assertEqual(pack_raw10p([0, 257, 514, 771]), bytes([0, 64, 128, 192, 228]))
        p = self.root / 'known.raw'
        p.write_bytes(bytes([0, 64, 128, 192, 228] * 2))
        run = self.invoke(frames=[p], stride='5')
        self.assertEqual(run.returncode, 0, run.stderr)
        channels = json.loads(run.stdout)['channels']
        self.assertEqual(channels['B']['mean_dn'], 257)
        self.assertEqual(channels['Gb']['mean_dn'], 514)
        self.assertEqual(channels['Gr']['mean_dn'], 257)
        self.assertEqual(channels['R']['mean_dn'], 514)
        self.assertIsNone(channels['B']['temporal_rms_dn'])

    def test_padded_two_frames_and_temporal_statistics(self):
        run = self.invoke('--hot-threshold', '20')
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        for channel, mean, count in [('B', 11, 0), ('Gb', 16, 1),
                                     ('Gr', 31, 2), ('R', 36, 2)]:
            item = report['channels'][channel]
            self.assertEqual(item['pixels_per_frame'], 2)
            self.assertEqual(item['sample_count'], 4)
            self.assertEqual(item['mean_dn'], mean)
            self.assertAlmostEqual(item['temporal_rms_dn'], math.sqrt(8))
            self.assertEqual(item['above_threshold_pixels'], count)
        self.assertEqual(report['frames'][0]['sha256'], hashlib.sha256(self.frames[0].read_bytes()).hexdigest())
        self.assertEqual(report['format']['stride_bytes'], 7)

    def test_roi_uses_full_frame_bayer_phase(self):
        run = self.invoke('--roi', '1,0,2,2')
        self.assertEqual(run.returncode, 0, run.stderr)
        channels = json.loads(run.stdout)['channels']
        self.assertEqual(channels['Gb']['mean_dn'], 11)
        self.assertEqual(channels['B']['mean_dn'], 16)
        self.assertEqual(channels['R']['mean_dn'], 31)
        self.assertEqual(channels['Gr']['mean_dn'], 36)
        for value in channels.values():
            self.assertEqual(value['pixels_per_frame'], 1)

    def test_all_bayer_orders(self):
        expected = {
            'BGGR': {'B': 11, 'Gb': 16, 'Gr': 31, 'R': 36},
            'RGGB': {'R': 11, 'Gr': 16, 'Gb': 31, 'B': 36},
            'GRBG': {'Gr': 11, 'R': 16, 'B': 31, 'Gb': 36},
            'GBRG': {'Gb': 11, 'B': 16, 'R': 31, 'Gr': 36},
        }
        for bayer, channels in expected.items():
            with self.subTest(bayer=bayer):
                run = self.invoke('--bayer', bayer)
                self.assertEqual(run.returncode, 0, run.stderr)
                got = json.loads(run.stdout)['channels']
                self.assertEqual({c: got[c]['mean_dn'] for c in channels}, channels)

    def test_u16_and_clipping(self):
        p = self.root / 'u16.raw'
        p.write_bytes(struct.pack('<8H', 1023, 1, 2, 3, 4, 5, 6, 7))
        run = self.invoke(frames=[p], packing='u16le-lsb', stride='8')
        self.assertEqual(run.returncode, 0, run.stderr)
        data = json.loads(run.stdout)
        self.assertEqual(data['channels']['B']['clipped_samples_at_1023'], 1)
        self.assertEqual(data['channels']['B']['mean_dn'], 512.5)
        p.write_bytes(struct.pack('<8H', 1024, 1, 2, 3, 4, 5, 6, 7))
        self.assertEqual(self.invoke(frames=[p], packing='u16le-lsb', stride='8').returncode, 2)

    def test_bad_size_stride_roi_and_duplicate(self):
        short = self.root / 'short.raw'
        short.write_bytes(b'\x00' * 13)
        long = self.root / 'long.raw'
        long.write_bytes(b'\x00' * 15)
        for run in (self.invoke(frames=[short]), self.invoke(frames=[long]),
                    self.invoke('--roi', '3,0,2,2'), self.invoke(stride='4'),
                    self.invoke(frames=[self.frames[0], self.frames[0]]),
                    self.invoke('--hot-threshold', '1024')):
            self.assertEqual(run.returncode, 2, (run.stdout, run.stderr))
            self.assertEqual(run.stdout, '')

    def test_output_never_overwrites(self):
        path = self.root / 'result.json'
        path.write_text('keep', encoding='utf-8')
        run = self.invoke('--output', str(path))
        self.assertEqual(run.returncode, 2)
        self.assertEqual(path.read_text(encoding='utf-8'), 'keep')
        new_path = self.root / 'new.json'
        run = self.invoke('--output', str(new_path))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, '')
        self.assertEqual(json.loads(new_path.read_text())['schema_version'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
