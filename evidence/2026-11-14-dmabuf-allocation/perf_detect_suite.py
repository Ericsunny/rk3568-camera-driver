#!/usr/bin/env python3
"""Predeclared C,D,D,C,C,D full-recorder short comparisons, one camera at a time."""
import json
from pathlib import Path
import statistics
import subprocess
import sys

base = Path(__file__).resolve().parent
order = ['copy', 'dmabuf', 'dmabuf', 'copy', 'copy', 'dmabuf']
plan = {'order': order, 'groups': ['imx415', 'ov8858'], 'rounds_per_mode': 3,
        'scope': 'OSD and RGA/RKNN on; threshold 1.1, manual event; no network clients; includes process startup CPU'}
(base / 'performance-detect-plan.json').write_text(json.dumps(plan, indent=2) + '\n')
results = {}
for name, device, width, height, fps in [('imx415', '/dev/video5', 1920, 1080, 15),
                                        ('ov8858', '/dev/video14', 1280, 960, 30)]:
    rows = []
    for n, mode in enumerate(order):
        out = base / ('perf-detect-%s-%02d-%s' % (name, n, mode))
        subprocess.run(['python3', str(base / 'run_recorder.py'), str(base / 'candidate-v2/evrec'),
                        device, str(width), str(height), str(fps), mode, str(out), '--detect'], check=True)
        subprocess.run(['python3', str(base / 'validate_recording.py'), str(out),
                        str(base / 'candidate-v2/tools/camera_service.py'), str(width * height * 3 // 2)], check=True)
        validation = json.loads((out / 'validation.json').read_text())
        rows.append({'mode': mode, 'directory': out.name,
                     'cpu_ms_per_frame': validation['cpu_ms_per_processed_frame'],
                     'processed_frames': validation['processed_frames'],
                     'copy_bytes': validation['copy_bytes']})
    medians = {mode: statistics.median(row['cpu_ms_per_frame'] for row in rows if row['mode'] == mode)
               for mode in ('copy', 'dmabuf')}
    results[name] = {'rows': rows, 'median_cpu_ms_per_frame': medians,
                     'cpu_reduction_percent': (1 - medians['dmabuf'] / medians['copy']) * 100}
    (base / 'performance-detect-summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print('CAMERA_RESULT ' + json.dumps({name: results[name]}), flush=True)
print('performance suite complete', flush=True)
