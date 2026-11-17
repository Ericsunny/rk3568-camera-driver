#!/usr/bin/env python3
"""Offline paired A/B brightness analysis of an external ordinary video.

PTS brackets describe sampled visible states, not a calibrated photon clock.
No host callback timestamps enter the optical delay calculation.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import sys


def edges(times, values, threshold, confirm=2):
    """Bracket each persistent state change; keep ambiguous frames between ends."""
    state = None
    last_old = None
    pending = None
    start = None
    count = 0
    result = []
    for i, value in enumerate(values):
        observed = 1 if value >= threshold else -1 if value <= -threshold else 0
        if observed == state:
            last_old = i
            pending, start, count = None, None, 0
        elif observed == 0:
            pending, start, count = None, None, 0
        else:
            if observed == pending:
                count += 1
            else:
                pending, start, count = observed, i, 1
            if count >= confirm:
                if state is not None:
                    result.append({
                        'polarity': observed, 'last_old_index': last_old,
                        'first_new_index': start, 'confirmed_index': i,
                        'lo_s': times[last_old], 'hi_s': times[start],
                        'center_s': (times[last_old] + times[start]) / 2,
                    })
                state, last_old = observed, i
                pending, start, count = None, None, 0
    return result


def pair_edges(direct, returned, max_delay=3.0, min_pairs=8, max_spread=.25):
    """Allow unmatched clip ends, reject skipped interior or polarity mismatches."""
    candidates = []
    for offset in range(-len(direct), len(returned) + 1):
        pairs = []
        for i, d in enumerate(direct):
            j = i + offset
            if 0 <= j < len(returned):
                r = returned[j]
                delta = r['center_s'] - d['center_s']
                if d['polarity'] != r['polarity'] or not 0 <= delta <= max_delay:
                    pairs = []
                    break
                pairs.append({
                    'direct_edge': i, 'returned_edge': j, 'polarity': d['polarity'],
                    'delay_lo_ms': (r['lo_s'] - d['hi_s']) * 1000,
                    'delay_hi_ms': (r['hi_s'] - d['lo_s']) * 1000,
                    'delay_center_ms': delta * 1000,
                })
        if len(pairs) >= min_pairs:
            values = [p['delay_center_ms'] for p in pairs]
            candidates.append({'offset': offset, 'n': len(pairs),
                               'center_spread_ms': max(values) - min(values),
                               'median_center_ms': statistics.median(values),
                               'pairs': pairs})
    candidates.sort(key=lambda c: (-c['n'], c['center_spread_ms']))
    accepted = [c for c in candidates if c['center_spread_ms'] <= max_spread * 1000]
    if not accepted:
        return {'pass': False, 'reason': 'no full-sequence alignment within declared limits',
                'candidates': candidates}
    # An alternate complete alignment with comparable coverage is ambiguous.
    best = accepted[0]
    if any(c['n'] >= best['n'] - 2 for c in accepted[1:]):
        return {'pass': False, 'reason': 'multiple plausible sequence alignments',
                'candidates': candidates}
    centers = [p['delay_center_ms'] for p in best['pairs']]
    return {'pass': True, 'selected_offset': best['offset'], 'paired_edges': best['n'],
            'median_center_ms': statistics.median(centers),
            'min_center_ms': min(centers), 'max_center_ms': max(centers),
            'sampling_bracket_envelope_ms': [min(p['delay_lo_ms'] for p in best['pairs']),
                                             max(p['delay_hi_ms'] for p in best['pairs'])],
            'pairs': best['pairs'],
            'candidates': [{k:v for k,v in c.items() if k != 'pairs'} for c in candidates]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('video', type=Path)
    parser.add_argument('--rois', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--av-site', type=Path)
    parser.add_argument('--normalization', choices=('raw', 'robust-range'), default='raw',
                        help='robust-range maps each signal p10/p90 to -1/+1 before state thresholds')
    a = parser.parse_args()
    if a.av_site:
        sys.path.insert(0, str(a.av_site.resolve()))
    import av
    import numpy as np
    config = json.loads(a.rois.read_text(encoding='utf-8'))
    a.out.mkdir(mode=0o700)
    video_hash = hashlib.sha256(a.video.read_bytes()).hexdigest()
    times, rows = [], []
    keys = [f'{variant}_{side}_{cell}' for variant in ('primary', 'secondary')
            for side in ('direct', 'returned') for cell in ('A', 'B')]
    series = {f'{variant}_{side}': [] for variant in ('primary', 'secondary')
              for side in ('direct', 'returned')}
    dimensions = None
    with av.open(str(a.video)) as container:
        stream = container.streams.video[0]
        meta = {'codec': stream.codec_context.name, 'average_rate': str(stream.average_rate),
                'base_rate': str(stream.base_rate), 'time_base': str(stream.time_base)}
        for index, frame in enumerate(container.decode(stream)):
            if frame.pts is None:
                raise ValueError('missing frame PTS')
            when = float(frame.pts * frame.time_base)
            if times and when <= times[-1]:
                raise ValueError('non-monotonic frame PTS')
            picture = np.asarray(frame.to_image().convert('L'))
            shape = picture.shape
            if dimensions is not None and shape != dimensions:
                raise ValueError('changing video dimensions')
            dimensions = shape
            means = []
            for variant in ('primary', 'secondary'):
                for side in ('direct', 'returned'):
                    cells = []
                    for cell in ('A', 'B'):
                        x0,y0,x1,y1 = config[variant][side][cell]
                        if not 0 <= x0 < x1 <= shape[1] or not 0 <= y0 < y1 <= shape[0]:
                            raise ValueError('ROI outside decoded frame')
                        cells.append(float(picture[y0:y1,x0:x1].mean()))
                    means.extend(cells)
                    series[f'{variant}_{side}'].append((cells[0]-cells[1])/(sum(cells)+1))
            times.append(when)
            rows.append([index,frame.pts,when,*means,*[series[k][-1] for k in series]])
    if len(times) < 2:
        raise ValueError('too few decoded frames')
    calibration = {}
    analysis_series = {}
    for key, values in series.items():
        low, high = [float(v) for v in np.quantile(values, [.1, .9])]
        calibration[key] = {'p10': low, 'p90': high, 'span': high-low}
        if a.normalization == 'robust-range':
            if high-low < .3:
                raise ValueError('insufficient brightness-state separation: '+key)
            analysis_series[key] = [(v-(low+high)/2)/((high-low)/2) for v in values]
        else:
            analysis_series[key] = values
    with (a.out/'brightness.csv').open('x', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['index','pts','pts_s',*keys,*[k+'_contrast' for k in series],
                         *[k+'_analysis_value' for k in analysis_series]])
        writer.writerows([*row,*[v[i] for v in analysis_series.values()]] for i,row in enumerate(rows))
    variants = {}
    for variant in ('primary', 'secondary'):
        for threshold in config['contrast_thresholds']:
            ds = edges(times, analysis_series[f'{variant}_direct'], threshold,
                       config['minimum_consecutive_state_frames'])
            rs = edges(times, analysis_series[f'{variant}_returned'], threshold,
                       config['minimum_consecutive_state_frames'])
            alignment = pair_edges(ds, rs, config['max_pair_delay_s'],
                                   config['minimum_paired_edges'], config['max_pair_center_spread_s'])
            variants[f'{variant}@{threshold}'] = {'direct_edges': ds, 'returned_edges': rs,
                                                'alignment': alignment}
    primary = variants[f"primary@{config['primary_threshold']}"]
    delta = np.diff(times) * 1000
    result = {
        'schema': 1, 'video_sha256': video_hash, 'video_bytes': a.video.stat().st_size,
        'rois_sha256': hashlib.sha256(a.rois.read_bytes()).hexdigest(),
        'analyzer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'pyav': av.__version__, 'ffmpeg_libraries': av.library_versions, 'numpy': np.__version__,
        'encoded': meta, 'decoded_frames': len(times),
        'pts_first_s': times[0], 'pts_last_s': times[-1],
        'pts_step_ms': {'min': float(delta.min()), 'median': float(np.median(delta)),
                        'max': float(delta.max())},
        'pts_gaps_over_1_5_median': [i+1 for i,d in enumerate(delta) if d > 1.5*np.median(delta)],
        'contrast_range': {k: {'min':min(v), 'max':max(v)} for k,v in series.items()},
        'normalization': a.normalization, 'photometric_calibration': calibration,
        'primary': primary['alignment'], 'variants': variants,
        'all_sensitivity_alignments_pass': all(v['alignment']['pass'] for v in variants.values()),
        'scope': 'external ordinary video, direct visible A/B change to returned visible A/B change, this exact RTSP/player configuration only',
        'interval_scope': 'sample-state transition brackets in phone video PTS; not a complete calibrated physical uncertainty bound',
        'limitations': ['ordinary video PTS not independently calibrated to sensor exposure',
                        'phone/IMX415 rolling shutter, exposure and display scanout/response uncalibrated',
                        'ROI selected from preview frames; sensitivity and manual edge checks required',
                        'single short run; no optimization comparison or person-trigger timing proof'],
    }
    (a.out/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:result[k] for k in ('decoded_frames','pts_step_ms','primary','all_sensitivity_alignments_pass')},indent=2))
    return 0 if result['primary']['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
