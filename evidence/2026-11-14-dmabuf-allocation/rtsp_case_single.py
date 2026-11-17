#!/usr/bin/env python3
"""Two software RTSP clients overlap a controlled manual event; board loopback."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('device')
p.add_argument('width', type=int)
p.add_argument('height', type=int)
p.add_argument('fps', type=int)
p.add_argument('out')
p.add_argument('--detect', action='store_true')
p.add_argument('--port', type=int, default=18754)
a = p.parse_args()
base = Path(__file__).resolve().parent
out = Path(a.out)
cmd = ['python3', str(base / 'run_recorder.py'), str(base / 'candidate/evrec-dmabuf'),
       a.device, str(a.width), str(a.height), str(a.fps), 'dmabuf', str(out), '--rtsp', str(a.port)]
if a.detect:
    cmd += ['--detect']
clients, logs = [], []
controller = subprocess.Popen(cmd)
try:
    started = time.monotonic()
    while time.monotonic() - started < 30:
        log = out / 'run.log'
        ready = log.exists() and '预录就绪' in log.read_text(errors='replace')
        if ready and time.monotonic() - started >= 4:
            break
        if controller.poll() is not None:
            raise RuntimeError('recorder exited before clients started')
        time.sleep(.1)
    else:
        raise RuntimeError('recorder not ready')
    for n in range(2):
        args = ['ffmpeg', '-nostdin', '-v', 'info', '-rtsp_transport', 'tcp',
                '-hwaccel', 'none', '-c:v', 'h264',
                '-i', 'rtsp://127.0.0.1:%d/live' % a.port, '-map', '0:v:0',
                '-frames:v', '1000', '-vsync', '0', '-an', '-f', 'null', '-']
        log = (out / ('client-%d.log' % n)).open('wb')
        logs.append(log)
        clients.append((args, subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)))
    rows = []
    for n, (args, client) in enumerate(clients):
        rc = client.wait(timeout=25)
        logs[n].flush()
        data = (out / ('client-%d.log' % n)).read_text(errors='replace')
        frames = [int(v) for v in re.findall(r'frame=\s*(\d+)', data)]
        rows.append({'argv': args, 'rc': rc, 'decoded_frames': max(frames, default=0),
                     'native_h264': '(h264 (native)' in data})
    rc = controller.wait(timeout=90)
    source_log = (out / 'run.log').read_text(errors='replace')
    before_trigger = source_log.split('>>> 触发 seq=', 1)[0]
    overlap = ('>>> 触发 seq=' in source_log and
               all('rtsp c%d: 客户端开始播放' % n in before_trigger and
                   'rtsp c%d: 连接关闭' % n not in before_trigger for n in range(2)))
    result = {'controller_rc': rc, 'clients': rows, 'both_playing_at_trigger': overlap}
    (out / 'rtsp-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    if rc or not overlap or any(row['rc'] or row['decoded_frames'] < 20 or not row['native_h264'] for row in rows):
        raise SystemExit(1)
finally:
    for _, client in clients:
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)
    if controller.poll() is None:
        # Stop the producer first so the monitor can record its controlled exit.
        pid_file = out / 'producer.pid'
        if pid_file.exists():
            try:
                os.kill(int(pid_file.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass
        controller.wait(timeout=15)
    for log in logs:
        log.close()
