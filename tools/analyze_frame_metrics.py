#!/usr/bin/env python3
"""Validate bounded evrec observations and summarize board-side frame timings."""
import argparse
import csv
import json
from pathlib import Path
import statistics

COLUMNS = ('epoch,frame_seq,driver_sequence,buffer_index,buffer_flags,driver_timestamp_ns,'
           'dq_mono_ns,encode_done_mono_ns,input_return_mono_ns,qbuf_done_mono_ns,'
           'input_dmabuf,completed').split(',')
TIMESTAMP_MASK, MONOTONIC, SOURCE_MASK = 0xE000, 0x2000, 0x70000


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    at = .95 * (len(values) - 1)
    low = int(at)
    p95 = values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (at - low)
    return {'n': len(values), 'mean_ms': statistics.mean(values),
            'median_ms': statistics.median(values), 'p95_ms': p95, 'max_ms': values[-1]}


def analyze(path, warmup=30):
    path = Path(path)
    if warmup < 0:
        raise ValueError('negative warmup')
    meta = json.loads(Path(str(path) + '.meta').read_text(encoding='utf-8'))
    if meta.get('schema') != 1 or meta.get('clock') != 'CLOCK_MONOTONIC' or meta.get('write_error'):
        raise ValueError('missing schema/clock or failed observation write')
    with path.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != COLUMNS:
            raise ValueError('unexpected frame observation columns')
        rows = []
        for item in reader:
            row = {k: int(item[k]) for k in COLUMNS}
            if any(v < 0 for v in row.values()) or row['completed'] not in (0, 1) or row['input_dmabuf'] not in (0, 1):
                raise ValueError('invalid integer/boolean')
            rows.append(row)
    if not rows or len(rows) != meta.get('stored') or not 0 < len(rows) <= meta.get('capacity', 0) <= 4096:
        raise ValueError('invalid or missing row count')
    if meta.get('seen') != len(rows) or meta.get('truncated') is not False:
        raise ValueError('truncated observations cannot summarize the full run')
    completed = [r for r in rows if r['completed']]
    if len(completed) != meta.get('completed_total'):
        raise ValueError('completed count differs from metadata')
    previous, last_epoch, previous_dq = {}, 0, 0
    gaps = duplicates = 0
    sources = {}
    for row in rows:
        epoch, seq = row['epoch'], row['driver_sequence']
        if row['dq_mono_ns'] <= previous_dq:
            raise ValueError('non-increasing DQBUF observation clock')
        previous_dq = row['dq_mono_ns']
        if not last_epoch <= epoch <= meta.get('epochs', 0) or not epoch or seq > 0xFFFFFFFF:
            raise ValueError('invalid or decreasing capture epoch/driver sequence')
        last_epoch = epoch
        if epoch in previous:
            delta = (seq - previous[epoch]) & 0xFFFFFFFF
            if delta >= 0x80000000:
                raise ValueError('driver sequence moved backwards without a new capture epoch')
            duplicates += delta == 0
            gaps += max(delta - 1, 0)
        previous[epoch] = seq
        if row['completed']:
            dq, enc, ret, qbuf = (row[k] for k in ('dq_mono_ns', 'encode_done_mono_ns',
                                                  'input_return_mono_ns', 'qbuf_done_mono_ns'))
            if not 0 < dq <= enc or qbuf < dq:
                raise ValueError('invalid completed-frame time order')
            if row['input_dmabuf']:
                if not enc <= ret <= qbuf:
                    raise ValueError('DMA-BUF returned before matched encoder input')
            elif ret or qbuf > enc:
                raise ValueError('copy buffer order is inconsistent')
    # Skip warmup independently after each STREAMON; never compare driver sequences across it.
    selected, counts = [], {}
    for row in completed:
        epoch = row['epoch']
        counts[epoch] = counts.get(epoch, 0) + 1
        if counts[epoch] > warmup:
            selected.append(row)
    if len(selected) < 2:
        raise ValueError('insufficient completed frames after warmup')
    values = {k: [] for k in ('dq_to_encode_done', 'dq_to_qbuf', 'driver_timestamp_to_dq',
                              'driver_timestamp_to_encode_done', 'encode_done_to_qbuf')}
    unavailable = 0
    for row in selected:
        dq, enc, qbuf = row['dq_mono_ns'], row['encode_done_mono_ns'], row['qbuf_done_mono_ns']
        values['dq_to_encode_done'].append((enc - dq) / 1e6)
        values['dq_to_qbuf'].append((qbuf - dq) / 1e6)
        if row['input_dmabuf']:
            values['encode_done_to_qbuf'].append((qbuf - enc) / 1e6)
        flags, driver = row['buffer_flags'], row['driver_timestamp_ns']
        clock_ok = (flags & TIMESTAMP_MASK) == MONOTONIC and driver > 0
        source = {0: 'EOF-flag', 0x10000: 'SOE-flag'}.get(flags & SOURCE_MASK, 'other-source-flag')
        key = ('monotonic/' if clock_ok else 'unusable-clock/') + source
        sources[key] = sources.get(key, 0) + 1
        if not clock_ok:
            unavailable += 1
            continue
        if driver > dq:
            raise ValueError('driver timestamp is later than DQBUF observation')
        values['driver_timestamp_to_dq'].append((dq - driver) / 1e6)
        values['driver_timestamp_to_encode_done'].append((enc - driver) / 1e6)
    first, last = selected[0]['dq_mono_ns'], selected[-1]['dq_mono_ns']
    if last <= first:
        raise ValueError('non-increasing DQBUF interval')
    return {'schema': 1, 'rows': len(rows), 'completed': len(completed),
            'incomplete': len(rows) - len(completed), 'warmup_per_epoch': warmup,
            'selected': len(selected), 'capture_epochs': sorted(previous),
            'driver_sequence_gaps': gaps, 'driver_sequence_duplicates': duplicates,
            'timestamp_sources': sources, 'driver_age_unavailable': unavailable,
            'observed_dq_fps': (len(selected) - 1) * 1e9 / (last - first),
            'timings': {k: distribution(v) for k, v in values.items()},
            'scope': 'board-side observations; driver timestamp position requires BSP verification; not display latency',
            'p95_method': 'linear interpolation at 0.95*(n-1)',
            'warmup_note': 'warmup removed per epoch; overall DQ fps includes any intervening recovery time'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('csv', type=Path)
    p.add_argument('--warmup', type=int, default=30)
    p.add_argument('-o', '--output', type=Path)
    a = p.parse_args()
    try:
        result = analyze(a.csv, a.warmup)
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as exc:
        p.exit(1, 'frame observation rejected: %s\n' % exc)
    text = json.dumps(result, indent=2) + '\n'
    if a.output:
        with a.output.open('x', encoding='utf-8') as stream:
            stream.write(text)
    print(text, end='')


if __name__ == '__main__':
    main()
