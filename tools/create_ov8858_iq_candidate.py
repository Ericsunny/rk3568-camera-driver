#!/usr/bin/env python3
"""Create an explicitly uncalibrated OV8858 reference-IQ candidate.

The reference must be supplied by the operator. Only CISFlip is changed to
match the fixed sensor mode orientation; no lens calibration is asserted.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re


def create(source, target):
    raw = source.read_bytes()
    text = raw.decode('utf-8')
    original = json.loads(text)
    if original.get('sensor_calib', {}).get('CISFlip') != 3:
        raise ValueError('expected reference CISFlip=3')
    replacement, count = re.subn(r'("CISFlip"\s*:\s*)3\b', r'\g<1>1', text)
    if count != 1:
        raise ValueError('expected exactly one CISFlip field')
    expected = copy.deepcopy(original)
    expected['sensor_calib']['CISFlip'] = 1
    if json.loads(replacement) != expected:
        raise ValueError('unexpected IQ changes')
    data = replacement.encode('utf-8')
    with target.open('xb') as out:
        out.write(data)
    return hashlib.sha256(raw).hexdigest(), hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    try:
        before, after = create(args.reference, args.candidate)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        parser.exit(1, 'IQ candidate not created: %s\n' % exc)
    print('reference_sha256=%s\ncandidate_sha256=%s' % (before, after))
    print('CISFlip 3 -> 1; all other parsed fields unchanged; not calibrated')


if __name__ == '__main__':
    main()
