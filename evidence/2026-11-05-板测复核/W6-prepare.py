#!/usr/bin/env python3
"""Add per-frame RGA markers to the already generated W4 test-only copy."""
from pathlib import Path
import sys


def replace(text, old, new, expected):
    actual = text.count(old)
    if actual != expected:
        raise ValueError('expected %d occurrences, got %d' % (expected, actual))
    return text.replace(old, new)


def main():
    if len(sys.argv) != 3:
        raise ValueError('usage: W6-prepare.py W4_SOURCE NEW_SOURCE')
    text = Path(sys.argv[1]).read_text(encoding='utf-8')
    text = replace(text,
        '\t\t\tif (imresize(s, d) != IM_STATUS_SUCCESS) { fprintf(stderr, "W4 RGA failed\\n"); break; }',
        '\t\t\tfprintf(stderr, "W6_BEGIN:%d\\n", f);\n'
        '\t\t\tIM_STATUS w6_status = imresize(s, d);\n'
        '\t\t\tfprintf(stderr, "W6_END:%d:%d\\n", f, (int)w6_status);\n'
        '\t\t\tif (w6_status != IM_STATUS_SUCCESS) { fprintf(stderr, "W4 RGA failed\\n"); break; }', 2)
    text = replace(text,
        '\t\t\t\tIM_STATUS st = imresize(s, d);',
        '\t\t\t\tfprintf(stderr, "W6_BEGIN:%d\\n", f);\n'
        '\t\t\t\tIM_STATUS st = imresize(s, d);\n'
        '\t\t\t\tfprintf(stderr, "W6_END:%d:%d\\n", f, (int)st);', 1)
    with Path(sys.argv[2]).open('x', encoding='utf-8', newline='\n') as out:
        out.write(text)
    print('created isolated W6 source:', sys.argv[2])


if __name__ == '__main__':
    main()
