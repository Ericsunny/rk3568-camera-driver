#!/usr/bin/env python3
"""Offline RAW10 sample statistics; inputs are read-only, not device capture."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import sys


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def roi_value(value):
    try:
        fields = tuple(int(part) for part in value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI must be x,y,width,height") from exc
    if len(fields) != 4 or fields[0] < 0 or fields[1] < 0 or min(fields[2:]) <= 0:
        raise argparse.ArgumentTypeError("ROI must be nonnegative x,y and positive width,height")
    return fields


def unpack_row(row, width, packing):
    """Return exactly width 10-bit values; padding is excluded."""
    if packing == 'raw10p':
        pixels = []
        for offset in range(0, width * 5 // 4, 5):
            low = row[offset + 4]
            pixels.extend((row[offset + k] << 2) | ((low >> (2 * k)) & 3)
                          for k in range(4))
        return pixels
    pixels = []
    for offset in range(0, width * 2, 2):
        value = row[offset] | row[offset + 1] << 8
        if value > 1023:
            raise ValueError('u16le-lsb contains bits above bit 9; wrong layout or data')
        pixels.append(value)
    return pixels


def channel_at(bayer, x, y):
    row = bayer[(y % 2) * 2:(y % 2) * 2 + 2]
    channel = row[x % 2]
    if channel == 'G':
        return 'Gr' if 'R' in row else 'Gb'
    return channel


def evaluate(paths, width, height, stride, packing, bayer, roi, hot_threshold):
    if packing == 'raw10p' and width % 4:
        raise ValueError('raw10p width must be divisible by 4')
    row_bytes = width * 5 // 4 if packing == 'raw10p' else width * 2
    if stride < row_bytes:
        raise ValueError('stride is smaller than the packed row size')
    x0, y0, roi_width, roi_height = roi
    if x0 + roi_width > width or y0 + roi_height > height:
        raise ValueError('ROI lies outside the frame')
    if hot_threshold is not None and not 0 <= hot_threshold <= 1023:
        raise ValueError('hot threshold must be 0..1023 DN')
    if len(paths) > 64:
        raise ValueError('at most 64 frame files per run')

    names = ('R', 'Gr', 'Gb', 'B')
    totals = {c: {'pixels': 0, 'samples': 0, 'sum': 0, 'sum_sq': 0,
                  'clipped_samples': 0, 'temporal_var_sum': 0.0,
                  'above_threshold_pixels': 0} for c in names}
    hashes = [hashlib.sha256() for _ in paths]

    with ExitStack() as stack:
        streams = [stack.enter_context(open(p, 'rb')) for p in paths]
        before = [os.fstat(f.fileno()) for f in streams]
        ids = [(s.st_dev, s.st_ino) for s in before]
        if len(set(ids)) != len(ids):
            raise ValueError('input files must be distinct (duplicate file/inode)')
        for p, st in zip(paths, before):
            if not stat.S_ISREG(st.st_mode) or st.st_size != stride * height:
                raise ValueError(f'{p}: expected one regular frame of exactly {stride * height} bytes')

        for y in range(height):
            decoded = []
            for stream, digest in zip(streams, hashes):
                row = stream.read(stride)
                if len(row) != stride:
                    raise ValueError('input became truncated during reading')
                digest.update(row)  # includes padding, for exact input provenance
                if y0 <= y < y0 + roi_height:
                    decoded.append(unpack_row(row, width, packing))
            if not y0 <= y < y0 + roi_height:
                continue
            for x in range(x0, x0 + roi_width):
                samples = [frame[x] for frame in decoded]
                channel = totals[channel_at(bayer, x, y)]
                channel['pixels'] += 1
                channel['samples'] += len(samples)
                channel['sum'] += sum(samples)
                channel['sum_sq'] += sum(v * v for v in samples)
                channel['clipped_samples'] += sum(v == 1023 for v in samples)
                mean = sum(samples) / len(samples)
                if len(samples) > 1:
                    channel['temporal_var_sum'] += sum((v - mean) ** 2 for v in samples) / (len(samples) - 1)
                if hot_threshold is not None and mean > hot_threshold:
                    channel['above_threshold_pixels'] += 1

        after = [os.fstat(f.fileno()) for f in streams]
        for first, last in zip(before, after):
            if (first.st_size, first.st_mtime_ns, first.st_ino) != (last.st_size, last.st_mtime_ns, last.st_ino):
                raise ValueError('input changed while being read')

    result = {}
    for name, data in totals.items():
        n, pixels = data['samples'], data['pixels']
        mean = data['sum'] / n if n else None
        spatial_temporal_variance = max(0.0, data['sum_sq'] / n - mean ** 2) if n else None
        result[name] = {
            'pixels_per_frame': pixels,
            'sample_count': n,
            'mean_dn': mean,
            'pooled_std_dn': math.sqrt(spatial_temporal_variance) if n else None,
            'temporal_rms_dn': math.sqrt(data['temporal_var_sum'] / pixels)
                               if pixels and len(paths) > 1 else None,
            'clipped_samples_at_1023': data['clipped_samples'],
            'above_threshold_pixels': data['above_threshold_pixels'] if hot_threshold is not None else None,
        }
    return {
        'schema_version': 1,
        'format': {'packing': packing, 'bayer_at_frame_origin': bayer,
                   'width': width, 'height': height, 'stride_bytes': stride,
                   'roi': {'x': x0, 'y': y0, 'width': roi_width, 'height': roi_height}},
        'frames': [{'path': str(p), 'sha256': digest.hexdigest()} for p, digest in zip(paths, hashes)],
        'hot_threshold_dn_strictly_greater': hot_threshold,
        'channels': result,
        'interpretation': 'DN statistics only; dark fixed-scene and capture controls are not verified.',
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packing', required=True, choices=('raw10p', 'u16le-lsb'))
    parser.add_argument('--bayer', required=True, choices=('RGGB', 'BGGR', 'GBRG', 'GRBG'))
    parser.add_argument('--width', required=True, type=positive)
    parser.add_argument('--height', required=True, type=positive)
    parser.add_argument('--stride', required=True, type=positive, help='bytes per row including padding')
    parser.add_argument('--roi', type=roi_value, help='x,y,width,height in full-frame coordinates')
    parser.add_argument('--hot-threshold', type=int, help='count pixel temporal means strictly above DN')
    parser.add_argument('--output', type=Path, help='new JSON file; existing files are never overwritten')
    parser.add_argument('frames', nargs='+', type=Path, help='distinct one-frame files, same capture conditions')
    args = parser.parse_args(argv)
    try:
        report = evaluate(args.frames, args.width, args.height, args.stride, args.packing,
                          args.bayer, args.roi or (0, 0, args.width, args.height), args.hot_threshold)
        text = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + '\n'
        if args.output:
            with args.output.open('x', encoding='utf-8') as out:
                out.write(text)
        else:
            sys.stdout.write(text)
    except (OSError, ValueError) as exc:
        print(f'raw10_stats: {exc}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
