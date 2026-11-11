#!/usr/bin/env python3
"""Summarize 1280x960 tightly packed NV12 frames; no image is exported."""
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys


def quantile(hist, count, p):
    limit = (count - 1) * p // 100
    total = 0
    for value in range(256):
        total += hist[value]
        if total > limit:
            return value
    raise ValueError('empty histogram')


def main():
    path = Path(sys.argv[1])
    width, height, count = 1280, 960, 4
    ysize = width * height
    frame_size = ysize * 3 // 2
    if path.stat().st_size != frame_size * count:
        raise ValueError('unexpected byte count')
    result = []
    with path.open('rb') as f:
        for i in range(count):
            frame = f.read(frame_size)
            hist = Counter(frame[:ysize])
            result.append({'frame': i, 'sha256': hashlib.sha256(frame).hexdigest(),
                           'y_mean': sum(v * n for v, n in hist.items()) / ysize,
                           'y_p10': quantile(hist, ysize, 10), 'y_p50': quantile(hist, ysize, 50),
                           'y_p90': quantile(hist, ysize, 90), 'y_p99': quantile(hist, ysize, 99),
                           'y_ge250_fraction': sum(hist[k] for k in range(250, 256)) / ysize,
                           'y_le16_fraction': sum(hist[k] for k in range(17)) / ysize})
    print(json.dumps({'width': width, 'height': height, 'format': 'NV12', 'frames': result}, indent=2))


if __name__ == '__main__':
    main()
