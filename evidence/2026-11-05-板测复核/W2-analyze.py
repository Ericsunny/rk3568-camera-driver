#!/usr/bin/env python3
"""Read-only analysis of one recorded CIF frame; outputs are newly created."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) != 4:
        raise ValueError('usage: W2-analyze.py RAW10_STATS FIRST_FRAME OUTPUT_DIR')
    stats, frame, out = map(Path, sys.argv[1:])
    if not out.is_dir():
        raise ValueError('output directory must exist')
    data = frame.read_bytes()
    if len(data) != 2048 * 1224:
        raise ValueError('input is not exactly one recorded frame')
    summary = {'input_size': len(data), 'input_sha256': hashlib.sha256(data).hexdigest(),
               'assumption_for_active_bytes': '2040 bytes per row, 8 bytes padding',
               'zero_active_byte_rows': 0, 'first_non_u10_u16le': None}
    for y in range(1224):
        row = data[y * 2048:(y + 1) * 2048]
        if not any(row[:2040]):
            summary['zero_active_byte_rows'] += 1
        if summary['first_non_u10_u16le'] is None:
            for x in range(1024):
                value = row[2*x] | row[2*x+1] << 8
                if value > 1023:
                    summary['first_non_u10_u16le'] = {'row': y, 'word': x, 'value': value}
                    break
    cases = {
        'packed': ['--packing', 'raw10p', '--bayer', 'BGGR', '--width', '1632',
                   '--height', '1224', '--stride', '2048'],
        'u16_probe': ['--packing', 'u16le-lsb', '--bayer', 'BGGR', '--width', '1024',
                      '--height', '1224', '--stride', '2048'],
    }
    for name, options in cases.items():
        cmd = [sys.executable, str(stats)] + options + [str(frame)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        with (out / (name + '.stdout')).open('x') as f:
            f.write(result.stdout)
        with (out / (name + '.stderr')).open('x') as f:
            f.write(result.stderr)
        summary[name] = {'command': cmd, 'exit': result.returncode}
    with (out / 'analysis.json').open('x') as f:
        json.dump(summary, f, indent=2); f.write('\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['packed']['exit'] == 0 and summary['u16_probe']['exit'] == 2 else 2


if __name__ == '__main__':
    sys.exit(main())
