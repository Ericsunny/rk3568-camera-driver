#!/usr/bin/env python3
"""Supervise evrec and finalize closed events without blocking capture I/O."""
import argparse
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
        if output.exists() or pending.exists():
            raise ValueError("MP4 output or unfinished conversion already exists")
        first = float(data["first_ts_ms"])
        last = float(data["last_ts_ms"])
        if not math.isfinite(first) or not math.isfinite(last) or last <= first:
            raise ValueError("missing or invalid packet time span")
        count = frame_count(ffprobe, source)
        if count < 2:
            raise ValueError("at least two decoded frames required to estimate rate")
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


def main():
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
    seen = set()
    failed = False
    try:
        (args.run_dir / "producer.pid").write_text(str(child.pid) + "\n")
        while child.poll() is None:
            failed |= collect(args.run_dir, seen)
            if stopped_at is not None and time.monotonic() - stopped_at > 20:
                raise RuntimeError(f"recorder PID {child.pid} did not stop; inspect device state")
            time.sleep(0.2)
        failed |= collect(args.run_dir, seen, final=True)
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
        child.terminate()
        try:
            child.wait(timeout=20)
        except subprocess.TimeoutExpired:
            print(f"[service] recorder PID {child.pid} is still alive", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
