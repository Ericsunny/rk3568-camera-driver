#!/usr/bin/env python3
"""Count RKISP21 DRC/merge flags in isp_params_observer QBUF records."""
import argparse
from collections import defaultdict
import json
from pathlib import Path


def analyze(lines):
    devices = defaultdict(lambda: {'attempts': 0, 'mapped': 0, 'accepted': 0,
                                  'frames': [], 'drc_enable_update': 0,
                                  'drc_enabled_bit': 0, 'drc_config_update': 0,
                                  'merge_enable_update': 0, 'merge_enabled_bit': 0,
                                  'merge_config_update': 0})
    overflow = 0
    for line in lines:
        if line.startswith(('MAP_OVERFLOW', 'BUF_OVERFLOW')):
            overflow += 1
        if not line.startswith('t='):
            continue
        fields = dict(part.split('=', 1) for part in line.split())
        row = devices[fields['dev']]
        row['attempts'] += 1
        if fields['mapped'] != '1':
            continue
        row['mapped'] += 1
        if int(fields['rc']) != 0:
            continue
        row['accepted'] += 1
        row['frames'].append(int(fields['frame']))
        for bit, prefix in ((41, 'drc'), (27, 'merge')):
            for key, suffix in (('en_update', 'enable_update'),
                                ('ens', 'enabled_bit'), ('cfg_update', 'config_update')):
                row[prefix + '_' + suffix] += bool(int(fields[key], 16) & (1 << bit))
    for row in devices.values():
        frames = row.pop('frames')
        row['first_frame'] = frames[0] if frames else None
        row['last_frame'] = frames[-1] if frames else None
    return {'overflow_events': overflow, 'devices': dict(devices)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    args = parser.parse_args()
    with args.trace.open(encoding='utf-8') as f:
        result = analyze(f)
    print(json.dumps(result, indent=2))
    if result['overflow_events'] or not result['devices']:
        raise SystemExit(1)
    if any(d['attempts'] != d['mapped'] or d['mapped'] != d['accepted']
           for d in result['devices'].values()):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
