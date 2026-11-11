#!/usr/bin/env python3
"""Supervise evrec and finalize closed events without blocking capture I/O."""
import argparse
from collections import deque
import csv
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

from h264_to_mp4 import frame_count, remux


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def publish_json(path, record):
    pending = path.with_name(path.name + ".part")
    with pending.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    # Exclusive hard-link publication prevents accidental replacement.
    os.link(pending, path)
    pending.unlink()


def finalize(metadata, ffmpeg="ffmpeg", ffprobe="ffprobe"):
    source = metadata.with_suffix(".h264")
    output = metadata.with_suffix(".mp4")
    pending = metadata.with_suffix(".pending.mp4")
    record = {"schema": 1, "status": "failed", "source_json": metadata.name,
              "source_video": source.name, "output_video": output.name}
    try:
        data = json.loads(metadata.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("recorder metadata must be an object")
        if data.get("write_error") is not False:
            raise ValueError("recorder did not report a clean close")
        if type(data.get("bytes")) is not int or data["bytes"] <= 0:
            raise ValueError("invalid recorder byte count")
        if source.stat().st_size != data["bytes"]:
            raise ValueError("recorded video size differs from metadata")
        packet_frames = None
        if data.get('schema') == 2:
            if data.get('timestamp_source') not in ('v4l2-monotonic', 'dqbuf-monotonic'):
                raise ValueError('missing capture timestamp source')
            previous = None
            last_seq = None
            packet_bytes = 0
            packet_frames = 0
            with metadata.with_suffix('.packets.csv').open(encoding='utf-8', newline='') as stream:
                for row in csv.DictReader(stream):
                    seq = int(row['frame_seq'])
                    timestamp = float(row['capture_ms'])
                    media = float(row['media_ms'])
                    length = int(row['bytes'])
                    if not math.isfinite(timestamp) or not math.isfinite(media) or length <= 0:
                        raise ValueError('invalid packet timing or length')
                    if abs(timestamp - media) > .001 or (previous is not None and timestamp < previous):
                        raise ValueError('inconsistent packet timestamps')
                    if previous is None and row['decodable_start'] != '1':
                        raise ValueError('recording does not begin at an independent decoding point')
                    if seq != last_seq:
                        packet_frames += 1
                    last_seq, previous = seq, timestamp
                    packet_bytes += length
            if previous is None or packet_bytes != data['bytes']:
                raise ValueError('packet record does not match video byte count')
        if output.exists() or pending.exists():
            raise ValueError("MP4 output or unfinished conversion already exists")
        first = float(data["first_ts_ms"])
        last = float(data["last_ts_ms"])
        if not math.isfinite(first) or not math.isfinite(last) or last <= first:
            raise ValueError("missing or invalid packet time span")
        count = frame_count(ffprobe, source)
        if count < 2:
            raise ValueError("at least two decoded frames required to estimate rate")
        if packet_frames is not None and packet_frames != count:
            raise ValueError('packet frame count differs from decoded frames')
        rate = (Fraction(count - 1) * 1000 / Fraction(str(last - first))).limit_denominator(1000000)
        remux(source, pending, rate, ffmpeg, ffprobe, count)
        os.link(pending, output)
        pending.unlink()
        record.update(status="complete", event_id=data.get("event_id"),
                      segment_index=data.get("segment_index"),
                      close_reason=data.get("close_reason"), video_frames=count,
                      packet_time_span_ms=last - first,
                      playback_fps=str(rate),
                      timing="average-rate approximation; individual capture timestamps not preserved",
                      source_sha256=sha256(source), metadata_sha256=sha256(metadata),
                      output_sha256=sha256(output))
        if packet_frames is not None:
            record['packet_log_sha256'] = sha256(metadata.with_suffix('.packets.csv'))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        record["error"] = str(exc)
    publish_json(metadata.with_suffix(".delivery.json"), record)
    print("[delivery] " + record["status"] + " " + metadata.name +
          (": " + record["error"] if "error" in record else ""), flush=True)
    return record["status"] == "complete"


def collect(run_dir, seen, final=False):
    failed = False
    for metadata in sorted(run_dir.glob("event*.json")):
        if metadata.name.endswith((".delivery.json", ".pending.json")) or metadata in seen:
            continue
        # The recorder writes JSON after closing H.264. Wait for parseable JSON
        # while it runs; on exit malformed metadata is a delivery failure.
        try:
            json.loads(metadata.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            if not final:
                continue
        try:
            failed |= not finalize(metadata)
        except (OSError, ValueError) as exc:
            print(f"[delivery] cannot publish result for {metadata.name}: {exc}",
                  file=sys.stderr, flush=True)
            failed = True
        seen.add(metadata)
    return failed


class DeliveryWorker:
    """One bounded, isolated remux worker; the capture supervisor never waits on it."""

    def __init__(self, run_dir, limit=32, timeout=120, worker_command=None):
        self.run_dir = run_dir
        self.limit = limit
        self.timeout = timeout
        self.worker_command = worker_command or [sys.executable, str(Path(__file__).resolve()), '--finalize-one']
        self.seen = set()
        self.pending = deque()
        self.worker = None
        self.started = None
        self.failed = False

    def scan(self, final=False):
        for metadata in sorted(self.run_dir.glob('event*.json')):
            if metadata.name.endswith(('.delivery.json', '.pending.json')) or metadata in self.seen:
                continue
            try:
                json.loads(metadata.read_text(encoding='utf-8'))
            except (OSError, ValueError):
                if not final:
                    continue
            if len(self.pending) + bool(self.worker) >= self.limit:
                raise RuntimeError('delivery queue capacity exceeded')
            self.pending.append(metadata)
            self.seen.add(metadata)

    def step(self):
        if self.worker is not None:
            status = self.worker.poll()
            if status is None and time.monotonic() - self.started > self.timeout:
                print('[delivery] worker timed out; terminating its process group', file=sys.stderr, flush=True)
                self.terminate()
                self.failed = True
            elif status is not None:
                self.failed |= status != 0
                self.worker = None
        if self.worker is None and self.pending:
            metadata = self.pending.popleft()
            self.worker = subprocess.Popen(self.worker_command + [str(metadata)], start_new_session=True)
            self.started = time.monotonic()

    def terminate(self):
        if self.worker is None:
            return
        if self.worker.poll() is None:
            try:
                os.killpg(self.worker.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            self.worker.wait(timeout=2)
        except subprocess.TimeoutExpired:
            raise RuntimeError(f'delivery worker PID {self.worker.pid} is still alive; stop processing')
        self.worker = None

    def busy(self):
        return self.worker is not None or bool(self.pending)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--finalize-one':
        try:
            return 0 if finalize(Path(sys.argv[2])) else 1
        except (OSError, ValueError) as exc:
            print(f'[delivery] cannot publish result: {exc}', file=sys.stderr, flush=True)
            return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not args.run_dir.is_dir():
        parser.error("existing run directory and recorder command required")
    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            parser.error("missing executable: " + tool)
    child = subprocess.Popen(command)
    stopped_at = None

    def stop(signum, frame):
        nonlocal stopped_at
        if stopped_at is None:
            stopped_at = time.monotonic()
        if child.poll() is None:
            child.terminate()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    delivery = DeliveryWorker(args.run_dir)
    failed = False
    try:
        (args.run_dir / "producer.pid").write_text(str(child.pid) + "\n")
        while child.poll() is None:
            delivery.scan()
            delivery.step()
            if stopped_at is not None and time.monotonic() - stopped_at > 20:
                raise RuntimeError(f"recorder PID {child.pid} did not stop; inspect device state")
            time.sleep(0.2)
        delivery.scan(final=True)
        # Bound the entire shutdown, not merely each individual conversion.
        delivery_deadline = time.monotonic() + 120
        while delivery.busy():
            delivery.step()
            if time.monotonic() >= delivery_deadline:
                raise RuntimeError('delivery shutdown deadline exceeded')
            time.sleep(0.2)
        failed |= delivery.failed
        # A closed-video-without-JSON or .part is not a completed event.
        unfinished = list(args.run_dir.glob("event*.part")) + list(args.run_dir.glob("event*.pending.json"))
        missing = [p for p in args.run_dir.glob("event*.h264") if not p.with_suffix(".json").exists()]
        if unfinished or missing:
            print("[delivery] unfinished recording: " +
                  ", ".join(p.name for p in unfinished + missing), file=sys.stderr)
            failed = True
        return child.returncode if child.returncode and child.returncode > 0 else int(failed or child.returncode != 0)
    except (OSError, RuntimeError) as exc:
        print(f"[service] {exc}", file=sys.stderr, flush=True)
        try:
            delivery.terminate()
        except RuntimeError as stop_error:
            print(f'[service] {stop_error}', file=sys.stderr, flush=True)
        if child.poll() is None:
            child.terminate()
        try:
            child.wait(timeout=20)
        except subprocess.TimeoutExpired:
            print(f"[service] recorder PID {child.pid} is still alive", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
