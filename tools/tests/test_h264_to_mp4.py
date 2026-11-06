#!/usr/bin/env python3
"""Host integration check; requires FFmpeg with libx264 and FFprobe."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "h264_to_mp4.py"
FFMPEG = os.environ.get("CAMERA_TEST_FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("CAMERA_TEST_FFPROBE", "ffprobe")


class RemuxTest(unittest.TestCase):
    def test_round_trip_and_refusals(self):
        with tempfile.TemporaryDirectory(prefix="camera-remux-test-", dir=ROOT) as tmp:
            source = Path(tmp) / "input.h264"
            output = Path(tmp) / "output.mp4"
            subprocess.run([FFMPEG, "-v", "error", "-f", "lavfi", "-i",
                            "testsrc2=size=160x120:rate=10:duration=1", "-c:v",
                            "libx264", "-bf", "0", "-f", "h264", str(source)], check=True)
            cmd = [sys.executable, str(SCRIPT), str(source), str(output), "--fps", "10",
                   "--ffmpeg", FFMPEG, "--ffprobe", FFPROBE]
            first = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertIn("Decoded frame counts match: 10", first.stdout)
            original = output.read_bytes()
            self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), original)
            bad_rate = [sys.executable, str(SCRIPT), str(source), str(Path(tmp) / "bad.mp4"),
                        "--fps", "0", "--ffmpeg", FFMPEG, "--ffprobe", FFPROBE]
            self.assertNotEqual(subprocess.run(bad_rate, capture_output=True).returncode, 0)
            self.assertEqual(subprocess.run([FFMPEG, "-v", "error", "-i", str(output),
                                             "-f", "null", "-"], capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
