#!/usr/bin/env python3
"""Run one isolated W6 trial and stop on unverified RGA or new errors."""
import argparse
import os
from pathlib import Path
import re
import secrets
import signal
import subprocess
import sys
import time

KNOWN_IMX = 'imx415 1-001a: write reg array error'
KNOWN_RGA = 'librga fail to get driver version! Compatibility mode will be enabled.'
KNOWN_RGA_KERNEL = 'rga2: unknown ioctl cmd!'
FRAME_STALL_SECONDS = 8.0
ERROR = re.compile(r'\b(error|fail(?:ed|ure)?|timeout|oops|panic|bug|warn(?:ing)?)\b', re.I)
CPU = re.compile(r'((?:cpu|software).*(?:fallback|fall.?back))|((?:fallback|fall.?back).*(?:cpu|software))', re.I)
STAMP = re.compile(r'\[\s*(\d+(?:\.\d+)?)\]')
LEVEL = re.compile(r'^\s*\w+\s*:\s*(emerg|alert|crit|err|warn(?:ing)?)\s*:', re.I)


class TraceCheck:
    def __init__(self):
        self.fds = set()
        self.active = None
        self.hits = 0
        self.completed = 0
        self.first_pid = None
        self.version_query_failed = False
        self.compat_query_ok = False

    def line(self, line):
        opened = re.search(r'openat\(.*"/dev/rga".*\)\s*=\s*(\d+)(?:</dev/rga>)?', line)
        if opened:
            self.fds.add(int(opened.group(1)))
        closed = re.search(r'close\((\d+)</dev/rga(?:<[^>]*>)?>\)\s*=\s*0', line)
        if closed:
            self.fds.discard(int(closed.group(1)))
        begin = re.search(r'write\([^\n]*"W6_BEGIN:(\d+)\\n"', line)
        end = re.search(r'write\([^\n]*"W6_END:(\d+):(\d+)\\n"', line)
        if self.active == 0:
            version = re.search(r'ioctl\((0x[0-9a-fA-F]+|\d+),\s*0x801c7201,.*\)\s*=\s*-1 EINVAL', line)
            compat = re.search(r'ioctl\((0x[0-9a-fA-F]+|\d+),\s*0x601b,.*\)\s*=\s*(?:0x0|0)(?:\s|$)', line)
            if version and int(version.group(1), 0) in self.fds:
                self.version_query_failed = True
            if compat and int(compat.group(1), 0) in self.fds:
                self.compat_query_ok = True
        if begin and int(begin.group(1)) == 0:
            pid = re.match(r'^\s*(\d+)\s+', line)
            if pid:
                self.first_pid = int(pid.group(1))
        if begin:
            if self.active is not None or int(begin.group(1)) != self.completed:
                raise ValueError('unexpected RGA begin marker')
            self.active = int(begin.group(1)); self.hits = 0
        if self.active is not None:
            call = re.search(r'ioctl\((0x[0-9a-fA-F]+|\d+),\s*(0x5017|0x5018),.*\)\s*=\s*(0x0|0)(?:\s|$)', line)
            if call and int(call.group(1), 0) in self.fds:
                self.hits += 1
        if end:
            if self.active != int(end.group(1)) or int(end.group(2)) != 1 or not self.hits:
                raise ValueError('RGA call lacks successful hardware ioctl or returned failure')
            self.active = None; self.completed += 1

    def finish(self, expected):
        if self.active is not None or self.completed != expected:
            raise ValueError('RGA trace incomplete: %d/%d' % (self.completed, expected))


def kernel_problem(line, since):
    stamp = STAMP.search(line)
    if not stamp:
        return 'unparseable kernel error' if LEVEL.search(line) else None
    if float(stamp.group(1)) < since:
        return None
    if line.rstrip().endswith(KNOWN_IMX):
        return None
    if LEVEL.search(line) or ERROR.search(line) or CPU.search(line):
        return line.strip()
    return None


class KernelWatch:
    """Use token/PID-tagged kernel records, never compare different clocks."""
    def __init__(self, token):
        self.token = token
        self.started = False
        self.init_state = 'before'
        self.init_pid = None
        self.rga_count = 0

    def line(self, line):
        stamp = STAMP.search(line)
        message = line[stamp.end():].strip() if stamp else ''
        if message == 'W6_TRIAL:%s:START' % self.token:
            if self.started:
                return 'repeated W6 trial marker'
            self.started = True
            return None
        if not self.started:
            return None
        marker = re.fullmatch(r'W6_INIT:([0-9a-f]{32}):(\d+):(BEGIN|END)', message)
        if marker and marker.group(1) == self.token:
            pid = int(marker.group(2))
            if marker.group(3) == 'BEGIN' and self.init_state == 'before' and pid > 0:
                self.init_pid = pid
                self.init_state = 'inside'
            elif marker.group(3) == 'END' and self.init_state == 'inside' and pid == self.init_pid:
                self.init_state = 'after'
            else:
                return 'invalid or repeated W6 initialization marker'
            return None
        if self.token in message and ('W6_INIT:' in message or 'W6_TRIAL:' in message):
            return 'malformed W6 kernel marker'
        exact = re.fullmatch(r'\s*kern\s*:\s*err\s*:\s*\[\s*(\d+(?:\.\d+)?)\]\s*rga2: unknown ioctl cmd!\s*', line)
        if exact:
            self.rga_count += 1
            if self.rga_count > 1:
                return 'repeated RGA unknown ioctl kernel error'
            if self.init_state != 'inside':
                return 'RGA unknown ioctl outside initialization markers'
            return None
        return kernel_problem(line, float('-inf'))

    def validate(self, check, final=False):
        if final and (not self.started or self.init_state != 'after'):
            raise ValueError('missing W6 kernel initialization markers')
        if check.completed:
            if self.init_pid is not None and check.first_pid != self.init_pid:
                raise ValueError('kernel marker PID differs from traced process')
            if self.rga_count and not (check.version_query_failed and check.compat_query_ok):
                raise ValueError('RGA initialization lacks traced version/compatibility queries')
        elif final:
            raise ValueError('first frame did not complete')


def write_kmsg(message):
    data = ('<6>' + message + '\n').encode('ascii')
    fd = os.open('/dev/kmsg', os.O_WRONLY | os.O_CLOEXEC)
    try:
        if os.write(fd, data) != len(data):
            raise OSError('short W6 kernel marker write')
    finally:
        os.close(fd)


def app_problem(line):
    if line.strip() == KNOWN_RGA:
        return None
    if ERROR.search(line) or CPU.search(line):
        return line.strip()
    return None


class Tail:
    def __init__(self, path):
        self.path = path; self.offset = 0; self.pending = ''

    def lines(self, final=False):
        if not self.path.exists():
            return []
        with self.path.open('r', errors='replace') as stream:
            stream.seek(self.offset)
            data = stream.read(); self.offset = stream.tell()
        parts = (self.pending + data).split('\n')
        self.pending = parts.pop()
        if final and self.pending:
            parts.append(self.pending); self.pending = ''
        return parts


def proc_start(pid):
    value = Path('/proc/%d/stat' % pid).read_text()
    fields = value[value.rfind(')') + 2:].split()
    if fields[0] in ('D', 'Z', 'X'):
        raise ValueError('3A process is in state ' + fields[0])
    return fields[19]


def find_d_state():
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            value = (path / 'stat').read_text()
            fields = value[value.rfind(')') + 2:].split()
            if fields and fields[0] == 'D':
                return path.name
        except (OSError, IndexError):
            continue
    return None


def check_resources(root, temperature, max_temp, reserve_bytes, reserve_inodes):
    temp = int(temperature.read_text().strip())
    fs = os.statvfs(str(root))
    if temp >= max_temp: raise ValueError('temperature abort threshold reached')
    if fs.f_bavail * fs.f_frsize < reserve_bytes or fs.f_favail < reserve_inodes:
        raise ValueError('storage reserve reached')


def run(mode, root, binary, expected_boot, service_pid, duration, temperature, max_temp,
        reserve_bytes, reserve_inodes):
    out = root / mode
    out.mkdir(mode=0o700)
    trace = out / 'trace.log'; appout = out / 'stdout'; apperr = out / 'stderr'
    kern = out / 'kernel.log'; kernerr = out / 'kernel.stderr'
    service_start = proc_start(service_pid)
    since = time.clock_gettime(time.CLOCK_BOOTTIME)
    token = secrets.token_hex(16)
    watch = KernelWatch(token)
    check = TraceCheck()
    tails = {p: Tail(p) for p in (trace, appout, apperr, kern)}
    reason = 'internal error'
    target = None; logger = None
    with kern.open('xb', buffering=0) as kl, kernerr.open('xb', buffering=0) as ke, \
         appout.open('xb', buffering=0) as ao, apperr.open('xb', buffering=0) as ae:
        try:
            check_resources(root, temperature, max_temp, reserve_bytes, reserve_inodes)
            if find_d_state(): raise ValueError('D-state process before trial')
            env = os.environ.copy(); env['RUNS'] = '1'; env['W6_TOKEN'] = token
            logger = subprocess.Popen(['stdbuf', '-oL', 'dmesg', '-x', '-w'],
                                      stdout=kl, stderr=ke, stdin=subprocess.DEVNULL, env=env)
            time.sleep(.2)
            if logger.poll() is not None:
                raise ValueError('kernel log monitor could not start')
            write_kmsg('W6_TRIAL:%s:START' % token)
            cmd = ['strace', '-f', '-ttt', '-yy', '-s', '160', '-e', 'trace=write,ioctl,openat,close',
                   '-e', 'raw=ioctl', '-o', str(trace), 'stdbuf', '-oL', '-eL', str(binary),
                   mode, '30', '/dev/video5']
            target = subprocess.Popen(cmd, cwd=str(out), env=env, stdout=ao, stderr=ae,
                                      stdin=subprocess.DEVNULL, start_new_session=True)
            deadline = time.monotonic() + duration
            next_d_check = 0
            last_progress = time.monotonic()
            prior_completed = 0
            while True:
                if logger.poll() is not None:
                    raise ValueError('kernel log monitor exited')
                if Path('/proc/sys/kernel/random/boot_id').read_text().strip() != expected_boot:
                    raise ValueError('boot changed')
                if proc_start(service_pid) != service_start:
                    raise ValueError('3A process identity changed')
                if time.monotonic() >= next_d_check:
                    d_pid = find_d_state()
                    if d_pid: raise ValueError('D-state process detected: ' + d_pid)
                    check_resources(root, temperature, max_temp, reserve_bytes, reserve_inodes)
                    next_d_check = time.monotonic() + .5
                for line in tails[kern].lines():
                    problem = watch.line(line)
                    if problem: raise ValueError('new kernel error: ' + problem)
                for path in (appout, apperr):
                    for line in tails[path].lines():
                        problem = app_problem(line)
                        if problem: raise ValueError('application error: ' + problem)
                for line in tails[trace].lines():
                    check.line(line)
                watch.validate(check)
                if check.completed != prior_completed:
                    prior_completed = check.completed
                    last_progress = time.monotonic()
                if time.monotonic() - last_progress >= FRAME_STALL_SECONDS:
                    raise ValueError('frame progress stalled for 8 seconds')
                if target.poll() is not None:
                    for line in tails[trace].lines(final=True): check.line(line)
                    for path in (appout, apperr):
                        for line in tails[path].lines(final=True):
                            problem = app_problem(line)
                            if problem: raise ValueError('application error: ' + problem)
                    if target.returncode != 0: raise ValueError('test process exit %d' % target.returncode)
                    check.finish(30)
                    watch.validate(check)
                    reason = 'complete; hardware submissions observed, hidden CPU work not excluded'
                    break
                if time.monotonic() >= deadline: raise ValueError('trial timeout')
                time.sleep(.02)
        except (OSError, ValueError) as exc:
            reason = str(exc)
        finally:
            if target is not None and target.poll() is None:
                try:
                    os.killpg(target.pid, signal.SIGTERM)
                    target.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    reason += '; trial did not terminate, no SIGKILL sent'
            if logger is not None and logger.poll() is None:
                logger.terminate()
                try: logger.wait(timeout=5)
                except subprocess.TimeoutExpired: reason += '; kernel monitor did not terminate'
        for line in tails[kern].lines(final=True):
            problem = watch.line(line)
            if problem and reason.startswith('complete;'):
                reason = 'new kernel error: ' + problem
        if reason.startswith('complete;'):
            try: watch.validate(check)
            except ValueError as exc: reason = str(exc)
    # A fresh snapshot catches records not drained by the follower at process exit.
    with (out / 'kernel-final.log').open('xb') as final_log:
        snapshot = subprocess.run(['dmesg', '-x'], stdout=final_log, stderr=subprocess.PIPE)
    if reason.startswith('complete;'):
        final_watch = KernelWatch(token)
        try:
            if snapshot.returncode:
                raise ValueError('final kernel snapshot failed')
            for line in (out / 'kernel-final.log').read_text(errors='replace').splitlines():
                problem = final_watch.line(line)
                if problem:
                    raise ValueError('new kernel error: ' + problem)
            final_watch.validate(check, final=True)
            watch = final_watch
        except ValueError as exc:
            reason = str(exc)
    with (out / 'monitor.txt').open('x') as f:
        f.write('mode=%s\nboot_time_start=%.6f\ntoken=%s\nobserved_rga_frames=%d\n'
                'init_pid=%s\ninit_state=%s\nrga_init_errors=%d\nreason=%s\n' %
                (mode, since, token, check.completed, watch.init_pid, watch.init_state,
                 watch.rga_count, reason))
    return 0 if reason.startswith('complete;') else 2


def main():
    def interrupted(sig, frame):
        del frame
        raise InterruptedError('monitor interrupted by signal %d' % sig)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', type=Path, required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--boot-id', required=True)
    p.add_argument('--service-pid', type=int, required=True)
    p.add_argument('--temperature', type=Path, required=True)
    p.add_argument('--max-temp', type=int, required=True)
    p.add_argument('--reserve-mib', type=int, default=2048)
    p.add_argument('--reserve-inodes', type=int, default=1000)
    p.add_argument('--timeout', type=int, default=60)
    a = p.parse_args()
    if (not a.run.is_absolute() or not a.binary.is_absolute() or not a.temperature.is_absolute()
            or not a.run.is_dir() or min(a.timeout, a.max_temp, a.reserve_mib, a.reserve_inodes) <= 0):
        p.error('absolute paths and positive bounds required')
    if Path('/proc/sys/kernel/random/boot_id').read_text().strip() != a.boot_id:
        raise ValueError('boot ID differs before launch')
    for mode in ('a', 'b', 'c'):
        print('START', mode, flush=True)
        if run(mode, a.run, a.binary, a.boot_id, a.service_pid, a.timeout, a.temperature,
               a.max_temp, a.reserve_mib * 1048576, a.reserve_inodes):
            print('STOP', mode, flush=True); return 2
        print('COMPLETE', mode, flush=True)
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError) as exc:
        print('W6 preflight failed:', exc, file=sys.stderr); sys.exit(2)
