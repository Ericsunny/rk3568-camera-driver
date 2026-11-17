#!/usr/bin/env python3
"""One idle-camera control/enum test and bounded ISP stream-switch test."""
import json
from pathlib import Path
import subprocess
import sys
import time

base = Path('/root/ov8858-driver-evaluation-20261114')
arm = sys.argv[1]
if arm not in ('self', 'vendor'):
    sys.exit('expected self or vendor')
out = base / arm
out.mkdir(mode=0o700)
summary = {'arm': arm, 'commands': {}, 'results': {}, 'switch_pass': 0, 'fatal': None}


def run(name, args, timeout=25, allow_failure=False):
    summary['commands'][name] = args
    with (out / (name + '.log')).open('wb') as log:
        p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                summary['fatal'] = name + ': process still alive after SIGTERM; stop'
            raise RuntimeError(name + ': timed out')
    summary['results'][name] = rc
    if rc and not allow_failure:
        raise RuntimeError(name + ': exit ' + str(rc))
    return rc


try:
    actual = Path('/sys/bus/i2c/devices/5-0036/driver').resolve().name
    expected = 'ov8858-sm' if arm == 'self' else 'ov8858'
    if actual != expected:
        raise RuntimeError('unexpected driver ' + actual)
    for dev in ('/dev/video5', '/dev/video14'):
        if subprocess.run(['fuser', dev], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0:
            raise RuntimeError('capture device busy: ' + dev)
    run('dmesg_before', ['dmesg'])
    run('date', ['date', '-Is'])
    run('uname', ['uname', '-a'])
    run('iq_restart', ['python3', str(base / 'observe_iq_restart.py')], timeout=45)
    run('three_a', ['pgrep', '-af', '^/usr/bin/rkaiq_3A_server([[:space:]]|$)'])
    run('enumeration', [str(base / 'enum_probe'), '/dev/v4l-subdev2'])
    run('tryfmt', [str(base / 'tryfmt_test'), '/dev/v4l-subdev2', '0'])
    run('sensor_before', ['v4l2-ctl', '-d', '/dev/v4l-subdev2', '--get-subdev-fmt=0'])
    run('controls_before', ['v4l2-ctl', '-d', '/dev/v4l-subdev2', '--list-ctrls'])
    run('set_same_sensor_mode', ['v4l2-ctl', '-d', '/dev/v4l-subdev2',
        '--set-subdev-fmt=pad=0,width=1632,height=1224,code=0x3007'])
    sensor = run('sensor_after', ['v4l2-ctl', '-d', '/dev/v4l-subdev2', '--get-subdev-fmt=0'])
    if '1632/1224' not in (out / 'sensor_after.log').read_text(errors='replace').replace(' ', ''):
        raise RuntimeError('sensor format after set not confirmed as 1632x1224')
    run('controls_after', ['v4l2-ctl', '-d', '/dev/v4l-subdev2', '--list-ctrls'])
    for i in range(1, 11):
        name = 'switch_%02d' % i
        run(name, [str(base / 'switch_probe')], timeout=20)
        if 'RESULT ok' not in (out / (name + '.log')).read_text(errors='replace'):
            raise RuntimeError(name + ': missing success marker')
        summary['switch_pass'] += 1
    run('post_three_a', ['pgrep', '-af', '^/usr/bin/rkaiq_3A_server([[:space:]]|$)'])
except (RuntimeError, OSError) as exc:
    summary['fatal'] = summary['fatal'] or str(exc)
finally:
    try:
        run('dmesg_after', ['dmesg'])
        before = (out / 'dmesg_before.log').read_text(errors='replace').splitlines()
        after = (out / 'dmesg_after.log').read_text(errors='replace').splitlines()
        overlap = next((n for n in range(min(len(before), len(after)), 0, -1)
                        if before[-n:] == after[:n]), 0)
        (out / 'dmesg_delta.log').write_text('\n'.join(after[overlap:]) + '\n')
        summary['dmesg_overlap_lines'] = overlap
    except (RuntimeError, OSError) as exc:
        summary['finish_error'] = str(exc)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(summary, ensure_ascii=False), flush=True)
sys.exit(1 if summary['fatal'] else 0)
