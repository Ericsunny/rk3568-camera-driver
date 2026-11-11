#!/usr/bin/env python3
"""Host integration check; requires FFmpeg with libx264 and FFprobe."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from h264_to_mp4 import frame_count

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "h264_to_mp4.py"
FFMPEG = os.environ.get("CAMERA_TEST_FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("CAMERA_TEST_FFPROBE", "ffprobe")


class RemuxTest(unittest.TestCase):
    def test_vendor_stdout_does_not_corrupt_stream_fields(self):
        result = subprocess.CompletedProcess([], 0,
            'librga initialization diagnostic\ncodec_name=h264|nb_read_frames=10\n', '')
        with patch('h264_to_mp4.subprocess.run', return_value=result):
            self.assertEqual(frame_count('ffprobe', Path('synthetic.h264')), 10)

    def test_non_utf8_library_diagnostic_and_stderr_failure(self):
        with tempfile.TemporaryDirectory(prefix='probe-output-') as tmp:
            probe = Path(tmp) / 'probe'
            probe.write_text('#!/usr/bin/env python3\nimport sys\n'
                             'sys.stdout.buffer.write(b"vendor \\xff\\ncodec_name=h264|nb_read_frames=10\\n")\n'
                             'if "bad" in sys.argv[-1]: sys.stderr.buffer.write(b"decode \\xff\\n")\n',
                             encoding='utf-8')
            probe.chmod(0o755)
            self.assertEqual(frame_count(str(probe), Path('ok.h264')), 10)
            with self.assertRaisesRegex(ValueError, 'decoder reported errors'):
                frame_count(str(probe), Path('bad.h264'))

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
