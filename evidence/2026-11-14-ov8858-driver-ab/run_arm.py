#!/usr/bin/env python3
"""Run one OV8858 driver arm; output images and recordings stay on the board."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

BASE = Path('/root/ov8858-driver-ab-20261114')
ARM = sys.argv[1]
assert ARM in ('self', 'vendor')
OUT = BASE / ARM
OUT.mkdir(mode=0o700)
RUN = OUT / 'run'
RUN.mkdir(mode=0o700)
IQ = Path('/etc/iqfiles/ov8858_DC-21AF-58-1474V1_unknown.json')
EXPECTED_IQ = '1dbb2b17c0244e90c2d35d5ccf88f56fb98c9a2d29be7a5ecd774fd8727cbb0f'
EXPECTED_APP = '40e9442b0e912df118f00df907375986804fb3e3f9e32eb52f2df7a8eb0b637b'
APP = BASE / 'evrec-test'
result = {'arm': ARM, 'steps': {}, 'commands': {}, 'fatal': None}
started = time.time()
proc = None


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(name, cmd, timeout=30, accepted=(0,)):
    result['commands'][name] = cmd
    start = time.monotonic()
    with (OUT / (name + '.log')).open('wb') as log:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log,
                             stderr=subprocess.STDOUT)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                result['fatal'] = name + ': process still alive after SIGTERM; stop testing'
            raise RuntimeError(name + ': timeout')
    result['steps'][name] = {'rc': rc, 'wall_seconds_including_setup': round(time.monotonic()-start, 3)}
    if rc not in accepted:
        raise RuntimeError(name + ': exit ' + str(rc))
    return (OUT / (name + '.log')).read_text(errors='replace')


def finish():
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            result['fatal'] = 'recorder still alive after SIGTERM; stop testing'
    try:
        run('dmesg_after', ['dmesg'])
        before = (OUT / 'dmesg_before.log').read_text(errors='replace').splitlines()
        after = (OUT / 'dmesg_after.log').read_text(errors='replace').splitlines()
        overlap = next((n for n in range(min(len(before), len(after)), 0, -1)
                        if before[-n:] == after[:n]), 0)
        (OUT / 'dmesg_delta.log').write_text('\n'.join(after[overlap:]) + '\n')
        result['dmesg_overlap_lines'] = overlap
        run('after_process', ['pgrep', '-af', '^/usr/bin/rkaiq_3A_server([[:space:]]|$)'])
        run('after_format', ['v4l2-ctl', '-d', '/dev/video14', '--get-fmt-video'])
    except (RuntimeError, OSError) as exc:
        result['finish_error'] = str(exc)
    result['elapsed_seconds'] = round(time.time() - started, 3)
    (OUT / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(result, ensure_ascii=False), flush=True)


try:
    if Path('/sys/bus/i2c/devices/5-0036/driver').resolve().name != ('ov8858-sm' if ARM == 'self' else 'ov8858'):
        raise RuntimeError('unexpected driver binding')
    if sha(IQ) != EXPECTED_IQ or sha(APP) != EXPECTED_APP:
        raise RuntimeError('IQ or application fingerprint changed')
    if subprocess.run(['fuser', '/dev/video14'], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode == 0:
        raise RuntimeError('ISP capture device already occupied')
    run('dmesg_before', ['dmesg'])
    run('uname', ['uname', '-a'])
    run('date', ['date', '-Is'])
    run('three_a', ['pgrep', '-af', '^/usr/bin/rkaiq_3A_server([[:space:]]|$)'])
    media = run('media0', ['media-ctl', '-d', '/dev/media0', '-p'])
    run('media2', ['media-ctl', '-d', '/dev/media2', '-p'])
    if '1632x1224' not in media or 'ov8858' not in media.lower():
        raise RuntimeError('sensor mode or topology does not match 1632x1224')
    sensor = re.search(r'entity \d+: [^\n]*ov8858[^\n]*\n[^\n]*\n\s*device node name (/dev/v4l-subdev\d+)', media, re.I)
    if not sensor:
        raise RuntimeError('cannot resolve sensor control node')
    run('controls_before', ['v4l2-ctl', '-d', sensor.group(1), '--list-ctrls'])
    run('capture90', ['v4l2-ctl', '-d', '/dev/video14',
        '--set-fmt-video=width=1632,height=1224,pixelformat=NV12',
        '--stream-mmap=4', '--stream-skip=20', '--stream-count=90', '--stream-to=/dev/null'])
    run('capture_frame', ['v4l2-ctl', '-d', '/dev/video14',
        '--set-fmt-video=width=1632,height=1224,pixelformat=NV12',
        '--stream-mmap=4', '--stream-skip=60', '--stream-count=1', '--stream-to=' + str(RUN / 'frame.nv12')])
    frame = (RUN / 'frame.nv12').read_bytes()
    result['frame_bytes'] = len(frame)
    result['frame_sha256'] = hashlib.sha256(frame).hexdigest()
    if len(frame) == 1632 * 1224 * 3 // 2:
        y = frame[:1632 * 1224]
        result['y_mean'] = round(sum(y) / len(y), 3)
    run('controls_after', ['v4l2-ctl', '-d', sensor.group(1), '--list-ctrls'])
    command = [str(APP), '-d', '/dev/video14', '-w', '1280', '-h', '960',
        '--fps', '30', '--gop', '30', '--pre', '2', '--post', '2', '--ring-mb', '24',
        '--kbps', '4000', '--recover', '1', '--rtsp', '18555', '--rtsp-clients', '1',
        '--max-events', '1', '--trigger', str(RUN / 'trigger'), '-o', str(RUN / 'event')]
    result['commands']['evrec'] = command
    with (OUT / 'evrec.log').open('wb') as log:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        time.sleep(4)
        if proc.poll() is not None:
            raise RuntimeError('recorder exited before RTSP connection')
        run('rtsp_client', ['ffmpeg', '-hide_banner', '-loglevel', 'info',
            '-rtsp_transport', 'tcp', '-c:v', 'h264', '-i', 'rtsp://127.0.0.1:18555/live',
            '-frames:v', '30', '-an', '-f', 'null', '-'], timeout=30)
        (RUN / 'trigger').touch()
        try:
            rc = proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            raise RuntimeError('event recorder did not finish within 20 seconds')
        result['steps']['evrec'] = {'rc': rc}
        if rc:
            raise RuntimeError('recorder exit ' + str(rc))
    videos = sorted(RUN.glob('event*.h264'))
    metadata = sorted(RUN.glob('event*.json'))
    result['video_count'] = len(videos)
    result['metadata_count'] = len(metadata)
    if len(videos) != 1 or len(metadata) != 1:
        raise RuntimeError('expected one H.264 and JSON pair')
    result['video_sha256'] = sha(videos[0])
    result['metadata_sha256'] = sha(metadata[0])
    run('event_decode', ['ffmpeg', '-v', 'error', '-xerror', '-c:v', 'h264',
        '-i', str(videos[0]), '-f', 'null', '-'], timeout=30)
    run('event_probe', ['ffprobe', '-v', 'error', '-count_frames',
        '-select_streams', 'v:0', '-show_entries', 'stream=width,height,nb_read_frames',
        '-of', 'json', str(videos[0])], timeout=30)
except (RuntimeError, OSError) as exc:
    result['fatal'] = result['fatal'] or str(exc)
finally:
    finish()
sys.exit(1 if result['fatal'] else 0)
