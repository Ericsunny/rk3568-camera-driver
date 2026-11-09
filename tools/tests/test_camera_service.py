#!/usr/bin/env python3
"""Linux host integration: synthetic H.264, real remux and recorder substitute."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from camera_service import collect, finalize, sha256
from h264_to_mp4 import frame_count


class DeliveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory(prefix="event-fixture-")
        cls.sample = Path(cls.fixture.name) / "sample.h264"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                        "testsrc2=size=160x120:rate=10:duration=1", "-c:v",
                        "libx264", "-bf", "0", "-f", "h264", str(cls.sample)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="event delivery ")
        self.run = Path(self.tmp.name)
        self.metadata = self.run / "event-01.json"
        self.video = self.metadata.with_suffix(".h264")
        self.video.write_bytes(self.sample.read_bytes())
        self.data = dict(write_error=False, bytes=self.video.stat().st_size,
                         first_ts_ms=1000, last_ts_ms=1900, duration_ms=900,
                         event_id=1, segment_index=0, close_reason="target_left")
        self.write_metadata()

    def tearDown(self):
        self.tmp.cleanup()

    def write_metadata(self):
        self.metadata.write_text(json.dumps(self.data), encoding="utf-8")

    def result(self):
        return json.loads(self.metadata.with_suffix(".delivery.json").read_text())

    def test_success_and_no_overwrite(self):
        original = self.metadata.read_bytes(), sha256(self.video)
        self.assertTrue(finalize(self.metadata))
        result = self.result()
        self.assertEqual(result["video_frames"], 10)
        self.assertEqual(result["playback_fps"], "10")
        self.assertIn("approximation", result["timing"])
        self.assertEqual(frame_count("ffprobe", self.video.with_suffix(".mp4")), 10)
        self.assertEqual(original, (self.metadata.read_bytes(), sha256(self.video)))
        output_hash = sha256(self.video.with_suffix(".mp4"))
        with self.assertRaises(FileExistsError):
            finalize(self.metadata)
        self.assertEqual(output_hash, sha256(self.video.with_suffix(".mp4")))

    def test_recording_error_and_existing_output(self):
        self.data["write_error"] = True
        self.write_metadata()
        self.assertFalse(finalize(self.metadata))
        self.assertEqual(self.result()["status"], "failed")
        self.assertFalse(self.video.with_suffix(".mp4").exists())

    def test_wrong_byte_count(self):
        self.data["bytes"] += 1
        self.write_metadata()
        self.assertFalse(finalize(self.metadata))
        self.assertIn("size", self.result()["error"])

    def test_corrupt_video(self):
        self.video.write_bytes(b"not an H264 stream")
        self.data["bytes"] = self.video.stat().st_size
        self.write_metadata()
        self.assertFalse(finalize(self.metadata))
        self.assertFalse(self.video.with_suffix(".mp4").exists())

    def test_remux_failure(self):
        self.assertFalse(finalize(self.metadata, ffmpeg="/bin/false"))
        self.assertEqual(self.result()["status"], "failed")

    def test_existing_mp4_preserved(self):
        output = self.video.with_suffix(".mp4")
        output.write_bytes(b"existing output")
        self.assertFalse(finalize(self.metadata))
        self.assertEqual(output.read_bytes(), b"existing output")

    def test_pending_and_incomplete_json(self):
        pending = self.metadata.with_suffix(".pending.json")
        self.metadata.rename(pending)
        seen = set()
        self.assertFalse(collect(self.run, seen))
        self.assertEqual(seen, set())
        pending.rename(self.metadata)
        self.metadata.write_text("{", encoding="utf-8")
        self.assertFalse(collect(self.run, seen))
        self.assertTrue(collect(self.run, seen, final=True))
        self.assertEqual(self.result()["status"], "failed")

    def test_bad_time_span(self):
        self.data["last_ts_ms"] = self.data["first_ts_ms"]
        self.write_metadata()
        self.assertFalse(finalize(self.metadata))
        self.assertIn("span", self.result()["error"])

    def test_service_exit_and_sigterm(self):
        for mode in ("normal", "fail", "sigterm", "orphan"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="service-") as tmp:
                run = Path(tmp)
                producer = run / "producer.py"
                producer.write_text('''import json, os, pathlib, signal, sys, time
run, sample, mode = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
video = run / 'event-01.h264'
video.write_bytes(sample.read_bytes())
data = dict(write_error=False, bytes=video.stat().st_size, first_ts_ms=1000, last_ts_ms=1900)
if mode != 'orphan':
    pending = run / 'event-01.pending.json'
    pending.write_text(json.dumps(data))
    pending.rename(run / 'event-01.json')
(run / 'ready').touch()
if mode == 'sigterm':
    while True: time.sleep(.1)
time.sleep(.5)
sys.exit(5 if mode == 'fail' else 0)
''', encoding="utf-8")
                command = [sys.executable, str(ROOT / "tools/camera_service.py"), str(run), "--",
                           sys.executable, str(producer), str(run), str(self.sample), mode]
                proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    if mode == "sigterm":
                        end = time.monotonic() + 8
                        while not (run / "ready").exists() and time.monotonic() < end:
                            time.sleep(.05)
                        self.assertTrue((run / "ready").exists())
                        proc.send_signal(signal.SIGTERM)
                    out, err = proc.communicate(timeout=20)
                    self.assertEqual(proc.returncode, {"normal": 0, "fail": 5, "sigterm": 0, "orphan": 1}[mode],
                                     (out, err))
                    if mode != "orphan":
                        self.assertEqual(json.loads((run / "event-01.delivery.json").read_text())["status"], "complete")
                    pid = int((run / "producer.pid").read_text())
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                finally:
                    if proc.poll() is None:
                        proc.terminate()
                        proc.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
