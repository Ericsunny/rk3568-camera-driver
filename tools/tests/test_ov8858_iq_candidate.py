#!/usr/bin/env python3
"""Synthetic reference only; no board or actual IQ calibration data."""
import json
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from create_ov8858_iq_candidate import create

with tempfile.TemporaryDirectory(prefix='iq-candidate-') as tmp:
    p=Path(tmp); source=p/'source.json'; target=p/'candidate.json'
    source.write_text('{"sensor_calib":{"CISFlip":3,"gain":128}}', encoding='utf-8')
    before=source.read_bytes()
    create(source,target)
    assert source.read_bytes()==before
    assert json.loads(target.read_text()) == {'sensor_calib':{'CISFlip':1,'gain':128}}
    try:
        create(source,target)
    except FileExistsError:
        pass
    else:
        raise AssertionError('existing candidate overwritten')
    source.write_text('{"sensor_calib":{"CISFlip":0}}', encoding='utf-8')
    try:
        create(source,p/'bad.json')
    except ValueError:
        pass
    else:
        raise AssertionError('unexpected source accepted')
    assert not (p/'bad.json').exists()
print('PASS IQ candidate preserves reference and changes only CISFlip')
