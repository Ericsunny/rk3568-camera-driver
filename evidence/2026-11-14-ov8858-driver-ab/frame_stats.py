#!/usr/bin/env python3
"""Summarize one retained NV12 frame per arm without transferring the image."""
from collections import Counter
import hashlib
import json
import math
from pathlib import Path

base = Path('/root/ov8858-driver-ab-20261114')
pixels = 1632 * 1224
for arm in ('self', 'vendor'):
    data = (base / arm / 'run' / 'frame.nv12').read_bytes()
    if len(data) != pixels * 3 // 2:
        raise ValueError('unexpected NV12 frame size: ' + arm)
    y = data[:pixels]
    hist = Counter(y)
    mean = sum(value * count for value, count in hist.items()) / pixels
    variance = sum((value - mean) ** 2 * count for value, count in hist.items()) / pixels
    result = {'pixels': pixels, 'sha256': hashlib.sha256(data).hexdigest(),
              'y_mean': round(mean, 3), 'y_std': round(math.sqrt(variance), 3),
              'y_min': min(y), 'y_max': max(y)}
    (base / arm / 'frame-stats.json').write_text(json.dumps(result, indent=2) + '\n')
    print(arm, result)
