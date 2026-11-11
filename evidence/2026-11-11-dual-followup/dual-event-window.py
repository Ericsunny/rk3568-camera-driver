#!/usr/bin/env python3
"""Run a bounded dual-camera event/RTSP observation in a private board directory."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

APP = Path('/root/camera-integration-20261110-dual-app')


def read(path):
    try:
        return Path(path).read_text(errors='replace')
    except OSError:
        return ''


def cmd(*args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('manual', 'detect'))
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    if not args.out.is_absolute():
        parser.error('output directory must be absolute')
    out = args.out
    out.mkdir(mode=0o700)
    run = out / 'run'
    plan = {'mode': args.mode, 'camera_command': ['python3', str(APP / 'tools/run_dual_camera.py'), args.mode, str(run)],
            'client_command': 'ffmpeg -nostdin -v error -xerror -threads 2 -c:v h264 -rtsp_transport tcp -i rtsp://127.0.0.1:PORT/live -progress pipe:1 -nostats -vsync 0 -f null -',
            'ports': {'imx415': 8554, 'ov8858': 8555}, 'sample_interval_s': 0.25,
            'duration_limit_s': 180,
            'stop_conditions': ['stop file', 'duration limit', 'unexpected child exit',
                                'sampled D for 3 s', 'no ISP mainpath progress for 5 s after first observation',
                                'missing first ISP frame by 12 s'],
            'criteria': 'For each camera, decoded frame counter increases while event .part exists; event is finalized and MP4 decodes. Detect mode additionally requires controlled no-person/person/leaving scene. No raw imagery exported.',
            'signals': 'create clients.start after warmup; create stop after event completion',
            'privacy': 'all camera media stays on board'}
    (out / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')
    if cmd('systemctl', 'is-active', '--quiet', 'rkaiq_3A.service').returncode:
        raise RuntimeError('3A inactive')
    if cmd('fuser', '/dev/video5', '/dev/video14').returncode == 0:
        raise RuntimeError('capture device busy')
    ports = cmd('ss', '-H', '-ltn', 'sport = :8554 or sport = :8555')
    if ports.returncode or ports.stdout.strip():
        raise RuntimeError('RTSP ports busy or ss failed')
    before = cmd('dmesg').stdout
    (out / 'dmesg-before.txt').write_text(before)
    (out / 'version.txt').write_text(cmd('uname', '-a').stdout + cmd('sha256sum', str(APP/'evrec'), str(APP/'tools/run_dual_camera.py'), str(APP/'tools/run_camera_demo.sh')).stdout)
    requested = None

    def stop(signum, _frame):
        nonlocal requested
        requested = 'signal ' + str(signum)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    pair = None
    clients = {}
    outputs = []
    d_since = {'imx415': None, 'ov8858': None}
    last_frame = {'imx415': None, 'ov8858': None}
    last_advance = {'imx415': None, 'ov8858': None}
    reason = None
    start = time.monotonic()
    try:
        log = (out / 'pair.log').open('w')
        outputs.append(log)
        pair = subprocess.Popen(plan['camera_command'], stdout=log, stderr=subprocess.STDOUT)
        with (out / 'samples.jsonl').open('w') as samples:
            while reason is None:
                now = time.monotonic()
                age = now - start
                if requested:
                    reason = requested
                elif (out/'stop').exists():
                    reason = 'stop file'
                elif age >= plan['duration_limit_s']:
                    reason = 'duration limit'
                elif pair.poll() is not None:
                    reason = 'pair exited early rc=' + str(pair.returncode)
                if (out/'clients.start').exists() and not clients and reason is None:
                    for name, port in plan['ports'].items():
                        progress = (out/(name+'-client-progress.txt')).open('w')
                        errors = (out/(name+'-client-errors.txt')).open('w')
                        outputs.extend([progress, errors])
                        command = ['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-threads', '2', '-c:v', 'h264',
                                   '-rtsp_transport', 'tcp', '-i', f'rtsp://127.0.0.1:{port}/live',
                                   '-progress', 'pipe:1', '-nostats', '-vsync', '0', '-f', 'null', '-']
                        clients[name] = subprocess.Popen(command, stdout=progress, stderr=errors)
                row = {'elapsed_s': round(age, 3), 'wall': datetime.datetime.now().astimezone().isoformat(), 'cameras': {}}
                for name, isp in (('imx415', 0), ('ov8858', 1)):
                    rawpid = read(run/name/'producer.pid').strip()
                    proc = read('/proc/'+rawpid+'/stat') if rawpid.isdigit() else ''
                    state = proc.rsplit(')', 1)[-1].strip().split(' ')[0] if ')' in proc else None
                    wchan = read('/proc/'+rawpid+'/wchan').strip() if state else None
                    match = re.search(r'Output\s+rkisp_mainpath[^\n]*\(frame:(\d+)', read('/proc/rkisp-vir'+str(isp)))
                    frame = int(match.group(1)) if match else None
                    progress = re.findall(r'^frame=\s*(\d+)', read(out/(name+'-client-progress.txt')), re.M)
                    parts = []
                    for path in (run/name).glob('event*.part'):
                        if not path.name.endswith('.packets.part'):
                            try:
                                parts.append({'name': path.name, 'bytes': path.stat().st_size})
                            except FileNotFoundError:
                                pass
                    row['cameras'][name] = {'pid': rawpid or None, 'state': state, 'wchan': wchan,
                                            'isp_frame': frame, 'decoded_frames': int(progress[-1]) if progress else 0,
                                            'event_parts': parts}
                    if state == 'D':
                        d_since[name] = d_since[name] if d_since[name] is not None else now
                        if now - d_since[name] >= 3:
                            reason = name + ' sampled D >=3 s'
                    else:
                        d_since[name] = None
                    if frame is not None and frame != last_frame[name]:
                        last_frame[name] = frame
                        last_advance[name] = now
                    if age > 12 and last_advance[name] is None:
                        reason = name + ' no ISP frame by 12 s'
                    if last_advance[name] is not None and now-last_advance[name] >= 5:
                        reason = name + ' ISP stalled 5 s'
                    if name in clients and clients[name].poll() is not None:
                        reason = name + ' client exited rc=' + str(clients[name].returncode)
                samples.write(json.dumps(row) + '\n')
                samples.flush()
                if reason is None:
                    time.sleep(plan['sample_interval_s'])
    finally:
        for client in clients.values():
            if client.poll() is None:
                client.send_signal(signal.SIGINT)
        for client in clients.values():
            try:
                client.wait(timeout=10)
            except subprocess.TimeoutExpired:
                reason = str(reason) + '; client did not stop'
        if pair and pair.poll() is None:
            pair.terminate()
            try:
                pair.wait(timeout=150)
            except subprocess.TimeoutExpired:
                reason = str(reason) + '; pair did not stop'
        for stream in outputs:
            stream.close()
        after = cmd('dmesg').stdout
        (out/'dmesg-after.txt').write_text(after)
        (out/'dmesg-delta.txt').write_text(after[len(before):] if after.startswith(before) else 'dmesg ring changed; compare full snapshots\n')
        status = {'reason': reason, 'pair_rc': pair.poll() if pair else None,
                  'client_rc': {name: proc.poll() for name, proc in clients.items()},
                  '3a': cmd('systemctl', 'is-active', 'rkaiq_3A.service').stdout.strip(),
                  'device_users': cmd('fuser', '/dev/video5', '/dev/video14').stdout.strip(),
                  'ended': datetime.datetime.now().astimezone().isoformat()}
        (out/'watch-status.json').write_text(json.dumps(status, indent=2) + '\n')
    return 0 if reason == 'stop file' and pair and pair.returncode == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
