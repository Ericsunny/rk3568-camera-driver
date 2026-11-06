#!/usr/bin/env python3
"""Wrap an Annex-B H.264 file in MP4 using an explicitly supplied frame rate.

Raw H.264 does not preserve evrec's per-packet timestamps. This helper makes
an approximate constant-frame-rate file; it does not reconstruct capture timing.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import shutil
import subprocess
import sys


def frame_count(probe: str, path: Path) -> int:
    result = subprocess.run(
        [probe, "-v", "error", "-count_frames", "-select_streams", "v:0",
         "-show_entries", "stream=codec_name,nb_read_frames", "-of", "json", str(path)],
        check=True, capture_output=True, text=True,
    )
    streams = json.loads(result.stdout)["streams"]
    if len(streams) != 1 or streams[0]["codec_name"] != "h264":
        raise ValueError("expected one selected H.264 video stream")
    count = int(streams[0]["nb_read_frames"])
    if count <= 0:
        raise ValueError("no decoded frames")
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="existing raw H.264 file")
    parser.add_argument("output", type=Path, help="new MP4 file (never overwritten)")
    parser.add_argument("--fps", required=True, help="assumed constant rate, e.g. 15 or 73/10")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable")
    parser.add_argument("--ffprobe", default="ffprobe", help="FFprobe executable")
    args = parser.parse_args()
    try:
        rate = Fraction(args.fps)
        if not 0 < rate <= 240:
            raise ValueError("fps must be between 0 and 240")
    except (ValueError, ZeroDivisionError):
        parser.error("--fps must be a positive number or fraction up to 240")
    if not args.input.is_file():
        parser.error("input is not an existing file")
    if args.output.exists():
        parser.error("output already exists")
    if args.output.suffix.lower() != ".mp4":
        parser.error("output must use the .mp4 extension")
    if not shutil.which(args.ffmpeg) or not shutil.which(args.ffprobe):
        parser.error("ffmpeg and ffprobe executables must be available")

    try:
        input_count = frame_count(args.ffprobe, args.input)
    except (subprocess.SubprocessError, ValueError, KeyError, IndexError, json.JSONDecodeError) as exc:
        print(f"Cannot count input frames: {exc}", file=sys.stderr)
        return 1

    print("Using an assumed constant frame rate; capture timing is not preserved.",
          file=sys.stderr, flush=True)
    command = [args.ffmpeg, "-nostdin", "-hide_banner", "-n", "-r", str(rate),
               "-f", "h264", "-i", str(args.input), "-map", "0:v:0", "-c:v", "copy",
               "-movflags", "+faststart", str(args.output)]
    rc = subprocess.run(command, check=False).returncode
    if rc:
        print("FFmpeg failed; any incomplete output must not be used.", file=sys.stderr)
        return rc
    try:
        output_count = frame_count(args.ffprobe, args.output)
    except (subprocess.SubprocessError, ValueError, KeyError, IndexError, json.JSONDecodeError) as exc:
        print(f"Cannot count output frames; do not use output: {exc}", file=sys.stderr)
        return 1
    if input_count != output_count:
        print(f"Frame count mismatch: input={input_count}, output={output_count}; do not use output.",
              file=sys.stderr)
        return 1
    print(f"Decoded frame counts match: {input_count}; timing remains approximate.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
