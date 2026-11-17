#!/usr/bin/env python3
"""Recompute software metrics from the published, data-only export."""
import csv
import hashlib
import json
from pathlib import Path
import statistics
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from analyze_frame_metrics import analyze, distribution


def read(name):
    return json.loads((HERE / name).read_text(encoding='utf-8'))


def near(actual, expected):
    assert abs(actual - expected) < 1e-7, (actual, expected)


def frames(folder, expected):
    result = analyze(HERE / folder / 'frame-metrics.csv')
    for key in ('rows', 'completed', 'driver_sequence_gaps', 'driver_sequence_duplicates'):
        assert result[key] == expected[key], (folder, key)
    near(result['observed_dq_fps'], expected['observed_dq_fps'])
    return result


def detections(folder):
    base = HERE / folder
    meta = json.loads((base / 'frame-metrics.csv.detect.meta').read_text())
    with (base / 'frame-metrics.csv').open(newline='') as f:
        frame_rows = list(csv.DictReader(f))
    selected = set()
    counts = {}
    for row in frame_rows:
        if row['completed'] != '1':
            continue
        epoch = int(row['epoch'])
        counts[epoch] = counts.get(epoch, 0) + 1
        if counts[epoch] > 30:
            selected.add((epoch, int(row['frame_seq']), int(row['driver_sequence'])))
    with (base / 'frame-metrics.csv.detect.csv').open(newline='') as f:
        rows = list(csv.DictReader(f))
    accepted = [r for r in rows if r['accepted'] == '1']
    assert len(accepted) == meta['applied']
    assert len(rows) == meta['stored'] and not meta['truncated'] and not meta['failed']
    ages = [(int(r['applied_ns']) - int(r['source_dq_ns'])) / 1e6
            for r in accepted if (int(r['epoch']), int(r['frame_seq']),
                                  int(r['driver_sequence'])) in selected]
    assert ages and min(ages) >= 0 and max(ages) <= 200
    return distribution(ages)


def main():
    manifest = read('provenance.json')
    for item in manifest['files']:
        data = (HERE / item['published']).read_bytes()
        assert len(data) == item['bytes']
        assert hashlib.sha256(data).hexdigest() == item['sha256'], item['published']
    published = read('published-source-hashes.json')
    if '--check-current-source' in sys.argv:
        for name, expected in published['files'].items():
            data = (ROOT / name).read_bytes().replace(b'\r\n', b'\n')
            assert hashlib.sha256(data).hexdigest() == expected, name
    abba = read('pipeline-abba.json')
    serial, pipeline = [], []
    for case in abba['cases']:
        if not case['name'].startswith('case-'):
            continue  # resource metadata has no raw CSV in this export
        result = frames('abba/' + case['name'], case['frame'])
        if 'pipeline' in case['name']:
            pipeline.append(result['observed_dq_fps'])
            detections('abba/' + case['name'])
        else:
            serial.append(result['observed_dq_fps'])
    near(statistics.mean(serial), abba['comparison']['serial_mean_fps'])
    near(statistics.mean(pipeline), abba['comparison']['pipeline_mean_fps'])
    person = read('person.json')
    frames('person', person['frame'])
    timing = detections('person')
    near(timing['p95_ms'], person['dq_to_apply_ms']['p95_ms'])
    assert person['gates']['strict_two_cycle_protocol'] is False
    cpu = read('encoder-cpu.json')
    values = {}
    for mode in ('copy', 'dmabuf'):
        runs = [r for r in cpu['cases'] if r['mode'] == mode]
        assert len(runs) == 4
        values[mode] = sum(r['cpu_total_ms'] for r in runs) / sum(r['completed'] for r in runs)
        near(values[mode], cpu['groups'][mode]['weighted_cpu_ms_per_completed'])
    reduction = (1 - values['dmabuf'] / values['copy']) * 100
    near(reduction, cpu['relative_reduction_percent'])
    print(json.dumps({'passed': True, 'verified_csv_meta_files': len(manifest['files']),
                      'serial_fps': statistics.mean(serial),
                      'pipeline_fps': statistics.mean(pipeline),
                      'encoder_cpu_reduction_percent': reduction,
                      'person_dq_to_apply_p95_ms': timing['p95_ms'],
                      'strict_two_cycle_protocol': False}, indent=2))


if __name__ == '__main__':
    main()
