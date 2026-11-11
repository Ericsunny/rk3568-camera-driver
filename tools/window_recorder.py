#!/usr/bin/env python3
"""Isolated, monotonic board-window recorder; completion is not a test verdict."""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import time


def durable(file):
    file.flush()
    os.fsync(file.fileno())


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def proc_start(pid):
    text = Path('/proc/%d/stat' % pid).read_text()
    # comm is parenthesized and may itself contain spaces or parentheses.
    return text[text.rfind(')') + 2:].split()[19]


def proc_values(pid):
    status = Path('/proc/%d/status' % pid).read_text().splitlines()
    fields = dict(line.split(':', 1) for line in status if ':' in line)
    pss = ''
    try:
        for line in Path('/proc/%d/smaps_rollup' % pid).read_text().splitlines():
            if line.startswith('Pss:'):
                pss = line.split()[1]
    except (OSError, PermissionError):
        pass
    return (fields.get('State', '').strip(), fields['VmRSS'].split()[0], pss,
            fields['Threads'].strip(), len(list(Path('/proc/%d/fd' % pid).iterdir())))


def mount_for(path):
    """Find the longest mounted prefix, decoding Linux mountinfo escapes."""
    path = os.path.realpath(path)
    matches = []
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        left, right = line.split(' - ', 1)
        mount = left.split()[4]
        for old, new in (('\\040', ' '), ('\\011', '\t'), ('\\012', '\n'), ('\\134', '\\')):
            mount = mount.replace(old, new)
        if path == mount or path.startswith(mount.rstrip('/') + '/'):
            matches.append((len(mount), mount, right.split()[0]))
    if not matches:
        raise ValueError('cannot determine filesystem')
    return max(matches)[1:]


def free_space(path):
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_frsize, stat.f_favail


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def timestamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--parent', required=True, help='existing persistent filesystem directory')
    p.add_argument('--duration', type=int, required=True, help='seconds')
    p.add_argument('--interval', type=int, default=60)
    p.add_argument('--trigger-every', type=int, required=True, help='SIGUSR1 cadence in seconds')
    p.add_argument('--first-trigger', type=int, required=True, help='warmup seconds')
    p.add_argument('--reserve-bytes', type=int, required=True)
    p.add_argument('--reserve-inodes', type=int, required=True)
    p.add_argument('--budget-bytes', type=int, required=True, help='calculated maximum run size')
    p.add_argument('--temperature', required=True, help='absolute sensor path, millidegrees C')
    p.add_argument('--max-temp', type=int, required=True, help='abort threshold, millidegrees C')
    p.add_argument('command', nargs=argparse.REMAINDER, help='after --; use {run} for private directory')
    a = p.parse_args(argv)
    if a.command and a.command[0] == '--':
        a.command.pop(0)
    if (not a.command or not os.path.isabs(a.command[0]) or not os.path.isabs(a.temperature)
            or min(a.duration, a.interval, a.trigger_every, a.first_trigger,
                   a.reserve_bytes, a.reserve_inodes, a.budget_bytes, a.max_temp) <= 0
            or a.first_trigger >= a.duration or a.trigger_every < a.interval):
        p.error('invalid positive bounds or command; executable and temperature must be absolute')
    if not any('{run}' in x for x in a.command):
        p.error('command must use {run} for the isolated output path')
    return a


def main(argv=None):
    a = parse_args(argv)
    parent = Path(a.parent).resolve(strict=True)
    mount, fstype = mount_for(parent)
    if (not parent.is_dir() or fstype in ('tmpfs', 'ramfs', 'devtmpfs') or
            str(parent) == '/boot' or str(parent).startswith('/boot/') or
            str(parent) == '/tmp' or str(parent).startswith('/tmp/')):
        raise ValueError('parent must be a persistent directory outside /tmp and /boot')
    free, inodes = free_space(parent)
    if free < a.reserve_bytes + a.budget_bytes or inodes < a.reserve_inodes:
        raise ValueError('insufficient preflight capacity or inodes')
    temp = int(Path(a.temperature).read_text().strip())
    if temp >= a.max_temp:
        raise ValueError('temperature at or above abort threshold')
    exe = Path(a.command[0]).resolve(strict=True)
    if not exe.is_file() or not os.access(str(exe), os.X_OK):
        raise ValueError('executable unavailable')
    run = parent / ('window-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + secrets.token_hex(4))
    run.mkdir(mode=0o700)
    (run / 'events').mkdir()
    command = [x.replace('{run}', str(run)) for x in a.command]
    manifest = {'command': command, 'boot_id': boot_id(), 'created_utc': timestamp(),
                'executable_sha256': sha256(exe), 'mount': mount, 'filesystem': fstype,
                'preflight_free_bytes': free, 'preflight_free_inodes': inodes,
                'duration': a.duration, 'interval': a.interval, 'trigger_every': a.trigger_every,
                'first_trigger': a.first_trigger, 'reserve_bytes': a.reserve_bytes,
                'reserve_inodes': a.reserve_inodes, 'budget_bytes': a.budget_bytes,
                'temperature': a.temperature, 'max_temp': a.max_temp}
    with (run / 'manifest.json').open('x') as f:
        json.dump(manifest, f, indent=2); f.write('\n'); durable(f)
    print(str(run), flush=True)
    stop = []
    def requested(sig, frame):
        del frame
        stop.append(sig)
    signal.signal(signal.SIGTERM, requested)
    signal.signal(signal.SIGINT, requested)
    child = None
    reason = 'internal error'
    trigger_count = 0
    missed = 0
    sampled = 0
    kernel_offset = 0
    kernel_tail = b''
    start = time.monotonic()
    expected_boot = manifest['boot_id']
    identity = None
    kernel = None
    try:
        with (run / 'service.log').open('xb', buffering=0) as log, \
             (run / 'kernel.log').open('xb', buffering=0) as kernel_log, \
             (run / 'samples.csv').open('x', newline='') as samples, \
             (run / 'triggers.jsonl').open('x') as triggers:
            writer = csv.writer(samples)
            writer.writerow(['utc', 'monotonic', 'elapsed', 'pid', 'start_ticks', 'state',
                             'rss_kib', 'pss_kib', 'threads', 'fds', 'free_bytes',
                             'free_inodes', 'temp_millic', 'boot_id'])
            durable(samples)
            dmesg = shutil.which('dmesg')
            if not dmesg:
                raise ValueError('dmesg unavailable')
            kernel = subprocess.Popen([dmesg, '--follow-new', '--nopager'],
                                      stdin=subprocess.DEVNULL, stdout=kernel_log,
                                      stderr=subprocess.STDOUT, close_fds=True)
            time.sleep(0.2)
            if kernel.poll() is not None:
                raise ValueError('kernel log follower failed to start; inspect kernel.log')
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, close_fds=True)
            identity = proc_start(child.pid)
            next_sample = start
            next_trigger = start + a.first_trigger
            while True:
                now = time.monotonic()
                if stop: reason = 'operator signal %d' % stop[0]; break
                if kernel.poll() is not None: reason = 'kernel log follower exited'; break
                if child.poll() is not None: reason = 'service exited early'; break
                if boot_id() != expected_boot or proc_start(child.pid) != identity:
                    reason = 'boot or process identity changed'; break
                if now >= start + a.duration:
                    reason = 'duration elapsed'; break
                if now >= next_sample:
                    if now >= next_sample + a.interval:
                        missed += int((now - next_sample) // a.interval)
                        next_sample += int((now - next_sample) // a.interval) * a.interval
                    free, inodes = free_space(run)
                    temp = int(Path(a.temperature).read_text().strip())
                    state, rss, pss, threads, fds = proc_values(child.pid)
                    writer.writerow([timestamp(), '%.6f' % now, '%.6f' % (now-start),
                                     child.pid, identity, state, rss, pss, threads, fds,
                                     free, inodes, temp, expected_boot]); durable(samples)
                    sampled += 1
                    with (run / 'kernel.log').open('rb') as check_kernel:
                        check_kernel.seek(kernel_offset)
                        kernel_text = kernel_tail + check_kernel.read().lower()
                        kernel_offset = check_kernel.tell()
                        kernel_tail = kernel_text[-32:]
                    if b'oops:' in kernel_text or b'kernel panic' in kernel_text:
                        reason = 'kernel Oops or panic detected'; break
                    if free < a.reserve_bytes or inodes < a.reserve_inodes:
                        reason = 'storage reserve reached'; break
                    if temp >= a.max_temp:
                        reason = 'temperature threshold reached'; break
                    if state.startswith('D'):
                        reason = 'service in D state'; break
                    next_sample += a.interval
                if now >= next_trigger:
                    if now >= next_trigger + a.trigger_every:
                        reason = 'missed trigger deadline'; break
                    # The PID is a child, and the identity was checked in this iteration.
                    os.kill(child.pid, signal.SIGUSR1)
                    trigger_count += 1
                    triggers.write(json.dumps({'utc': timestamp(), 'monotonic': now,
                                               'number': trigger_count}) + '\n'); durable(triggers)
                    next_trigger += a.trigger_every
                time.sleep(min(0.5, max(0.01, min(next_sample, next_trigger, start+a.duration)-now)))
    except (OSError, ValueError, KeyError, IndexError) as exc:
        reason = 'recorder error: ' + str(exc)
    finally:
        if kernel is not None and kernel.poll() is None:
            kernel.terminate()
            try: kernel.wait(timeout=5)
            except subprocess.TimeoutExpired: reason += '; kernel follower did not stop'
        if child is not None and child.poll() is None:
            try:
                if identity is None or proc_start(child.pid) == identity:
                    child.send_signal(signal.SIGTERM)
                    child.wait(timeout=15)
            except (OSError, subprocess.TimeoutExpired):
                reason += '; service did not stop cleanly (no SIGKILL sent)'
        if reason == 'duration elapsed':
            missed = max(missed, (a.duration + a.interval - 1) // a.interval - sampled)
        summary = {'reason': reason, 'elapsed': time.monotonic()-start, 'samples': sampled,
                   'sample_deadlines_missed': missed, 'triggers_sent': trigger_count,
                   'service_returncode': child.poll() if child is not None else None,
                   'finished_utc': timestamp(), 'verdict': 'NOT_EVALUATED'}
        try:
            with (run / 'summary.json').open('x') as f:
                json.dump(summary, f, indent=2); f.write('\n'); durable(f)
        except OSError as exc:
            print('unable to persist summary: %s' % exc, file=sys.stderr)
            return 2
    return 0 if reason == 'duration elapsed' and missed == 0 and child.returncode == 0 else 2


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print('preflight failed: %s' % exc, file=sys.stderr)
        sys.exit(2)
