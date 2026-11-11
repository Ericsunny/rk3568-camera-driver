#!/usr/bin/env python3
"""Record four RTSP/TCP clients autonomously; offline decoding is still required."""
import argparse
import datetime
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import time
from urllib.parse import urlsplit

from window_recorder import durable, free_space, mount_for, sha256


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--parent', required=True)
    p.add_argument('--url', required=True)
    p.add_argument('--duration', type=int, required=True)
    p.add_argument('--startup-timeout', type=int, default=60)
    p.add_argument('--sample-interval', type=int, default=1)
    p.add_argument('--reserve-bytes', type=int, required=True)
    p.add_argument('--budget-bytes', type=int, required=True)
    p.add_argument('--reserve-inodes', type=int, required=True)
    p.add_argument('--temperature', required=True)
    p.add_argument('--max-temp', type=int, required=True)
    a = p.parse_args()
    parsed = urlsplit(a.url)
    if (parsed.scheme != 'rtsp' or not parsed.hostname or parsed.username is not None or
            parsed.password is not None or parsed.query or parsed.fragment):
        p.error('URL must be plain rtsp://host/path, without credentials or query')
    if min(a.duration, a.startup_timeout, a.sample_interval, a.reserve_bytes,
           a.budget_bytes, a.reserve_inodes, a.max_temp) <= 0 or not os.path.isabs(a.temperature):
        p.error('bounds must be positive; temperature path must be absolute')
    parent = Path(a.parent).resolve(strict=True)
    mount, fstype = mount_for(parent)
    if (not parent.is_dir() or fstype in ('tmpfs', 'ramfs', 'devtmpfs') or
            str(parent) == '/tmp' or str(parent).startswith('/tmp/') or
            str(parent) == '/boot' or str(parent).startswith('/boot/')):
        raise ValueError('parent must be persistent and outside /tmp and /boot')
    free, inodes = free_space(parent)
    if free < a.budget_bytes + a.reserve_bytes or inodes < a.reserve_inodes:
        raise ValueError('insufficient capacity or inodes')
    if int(Path(a.temperature).read_text()) >= a.max_temp:
        raise ValueError('temperature threshold reached')
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise ValueError('ffmpeg unavailable')
    run = parent / ('rtsp4-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + secrets.token_hex(4))
    run.mkdir(mode=0o700)
    manifest = {'url': a.url, 'duration': a.duration, 'startup_timeout': a.startup_timeout,
                'sample_interval': a.sample_interval, 'reserve_bytes': a.reserve_bytes,
                'reserve_inodes': a.reserve_inodes, 'budget_bytes': a.budget_bytes,
                'temperature': a.temperature, 'max_temp': a.max_temp, 'mount': mount,
                'filesystem': fstype, 'ffmpeg': os.path.realpath(ffmpeg),
                'ffmpeg_sha256': sha256(ffmpeg), 'created_utc': utc()}
    with (run / 'manifest.json').open('x') as f:
        json.dump(manifest, f, indent=2); f.write('\n'); durable(f)
    print(str(run), flush=True)
    stop = []
    def requested(sig, frame):
        del frame
        stop.append(sig)
    signal.signal(signal.SIGTERM, requested)
    signal.signal(signal.SIGINT, requested)
    children, logs = [], []
    start = time.monotonic()
    first = [None] * 4
    last_growth = [None] * 4
    sizes = [0] * 4
    t0 = None
    reason = 'internal error'
    try:
        with (run / 'samples.jsonl').open('x') as samples:
            for i in range(4):
                log = (run / ('client%d.log' % (i+1))).open('xb', buffering=0)
                logs.append(log)
                target = run / ('client%d.ts' % (i+1))
                cmd = [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'warning',
                       '-rtsp_transport', 'tcp', '-i', a.url, '-map', '0:v:0',
                       '-c', 'copy', '-f', 'mpegts', '-n', str(target)]
                children.append(subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log,
                                                 stderr=subprocess.STDOUT, close_fds=True))
            next_sample = start
            while True:
                now = time.monotonic()
                if stop: reason = 'operator signal %d' % stop[0]; break
                if any(c.poll() is not None for c in children):
                    reason = 'client exited early'; break
                if t0 is None and now >= start + a.startup_timeout:
                    reason = 'startup timeout'; break
                if t0 is not None and now >= t0 + a.duration:
                    reason = 'duration elapsed'; break
                if now >= next_sample:
                    if now >= next_sample + a.sample_interval:
                        reason = 'sample deadline missed'; break
                    free, inodes = free_space(run)
                    temp = int(Path(a.temperature).read_text())
                    for i in range(4):
                        target = run / ('client%d.ts' % (i+1))
                        size = target.stat().st_size if target.exists() else 0
                        if size < sizes[i]:
                            raise ValueError('client output shrank')
                        if size > sizes[i]:
                            last_growth[i] = now
                            if first[i] is None: first[i] = now
                        sizes[i] = size
                    if t0 is None and all(x is not None for x in first): t0 = now
                    samples.write(json.dumps({'utc': utc(), 'monotonic': now, 'sizes': sizes,
                                              'pids': [c.pid for c in children], 'free_bytes': free,
                                              'free_inodes': inodes, 'temp_millic': temp}) + '\n')
                    durable(samples)
                    if free < a.reserve_bytes or inodes < a.reserve_inodes:
                        reason = 'storage reserve reached'; break
                    if temp >= a.max_temp: reason = 'temperature threshold reached'; break
                    # Byte growth is a warning signal, not a decoded-frame check.
                    if t0 is not None and any(now - x >= 5 for x in last_growth):
                        reason = 'client output stagnant for 5 seconds'; break
                    next_sample += a.sample_interval
                time.sleep(min(0.2, max(0.01, next_sample-now)))
    except (OSError, ValueError) as exc:
        reason = 'recorder error: ' + str(exc)
    finally:
        for child in children:
            if child.poll() is None:
                try: child.send_signal(signal.SIGINT)
                except OSError: pass
        for child in children:
            try: child.wait(timeout=15)
            except subprocess.TimeoutExpired: reason += '; client did not stop (no SIGKILL sent)'
        for log in logs: log.close()
        summary = {'reason': reason, 'elapsed': time.monotonic()-start,
                   'provisional_t0_monotonic': t0, 'first_growth_monotonic': first,
                   'client_returncodes': [c.poll() for c in children],
                   'finished_utc': utc(), 'verdict': 'NOT_EVALUATED'}
        try:
            with (run / 'summary.json').open('x') as f:
                json.dump(summary, f, indent=2); f.write('\n'); durable(f)
        except OSError as exc:
            print('unable to persist summary: %s' % exc, file=sys.stderr)
            return 2
    return 0 if reason == 'duration elapsed' and all(c.returncode in (0, 255) for c in children) else 2


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError) as exc:
        print('preflight failed: %s' % exc, file=sys.stderr)
        sys.exit(2)
