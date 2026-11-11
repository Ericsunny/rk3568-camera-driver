#!/usr/bin/env python3
"""Create a separate RKISP21 DRC IQ variant; never overwrite an IQ file.

The output is experimental reference data, not lens/sensor calibration.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def create(source, output, gain=None):
    if gain is not None and (not math.isfinite(gain) or not 1 <= gain <= 8):
        raise ValueError('test DRC gain must be within [1, 8]')
    original_bytes = source.read_bytes()
    original = json.loads(original_bytes)
    candidate = copy.deepcopy(original)
    scenes = candidate['main_scene']
    if len(scenes) != 1 or scenes[0]['name'] != 'normal':
        raise ValueError('expected exactly one normal main scene')
    sub = scenes[0]['sub_scene']
    if len(sub) != 1 or sub[0]['name'] != 'day':
        raise ValueError('expected exactly one day sub-scene')
    drc = sub[0]['scene_isp21']['adrc_calib']['DrcTuningPara']
    if type(drc.get('Enable')) not in (int, bool) or drc['Enable'] not in (0, 1):
        raise ValueError('expected DRC Enable to be 0 or 1')
    old_enable = drc['Enable']
    drc['Enable'] = 1
    old_gain = None
    if gain is not None:
        table = drc['DrcGain']
        old_gain = table['DrcGain']
        if (not isinstance(old_gain, list) or not old_gain or
                table.get('DrcGain_len') != len(old_gain) or
                any(isinstance(v, bool) or not isinstance(v, (int, float)) or
                    not math.isfinite(v) for v in old_gain)):
            raise ValueError('invalid DRC gain table')
        table['DrcGain'] = [gain] * len(old_gain)
    check = copy.deepcopy(candidate)
    check_drc = check['main_scene'][0]['sub_scene'][0]['scene_isp21']['adrc_calib']['DrcTuningPara']
    check_drc['Enable'] = old_enable
    if gain is not None:
        check_drc['DrcGain']['DrcGain'] = old_gain
    if check != original:
        raise ValueError('unexpected modification outside DRC fields')
    payload = (json.dumps(candidate, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')
    with output.open('xb') as f:
        f.write(payload)
    return sha256(original_bytes), sha256(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('output', type=Path, help='new file; never overwritten')
    parser.add_argument('--gain', type=float, help='optional experimental DRC gain in [1, 8]')
    args = parser.parse_args()
    try:
        before, after = create(args.reference, args.output, args.gain)
    except (OSError, ValueError, KeyError, TypeError, IndexError) as exc:
        parser.exit(1, f'IQ variant not created: {exc}\n')
    print(f'reference_sha256={before}\nvariant_sha256={after}')
    print('DRC enabled in normal/day scene; reference data is not sensor/lens calibration')
    if args.gain is not None:
        print(f'experimental DrcGain={args.gain:g}; image quality not validated')


if __name__ == '__main__':
    main()
