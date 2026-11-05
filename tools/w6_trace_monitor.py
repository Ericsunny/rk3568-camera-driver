#!/usr/bin/env python3
"""Run one isolated W6 trial and stop on unverified RGA or new errors."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

KNOWN_IMX = 'imx415 1-001a: write reg array error'
KNOWN_RGA = 'librga fail to get driver version! Compatibility mode will be enabled.'
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

    def line(self, line):
        opened = re.search(r'openat\(.*"/dev/rga".*\)\s*=\s*(\d+)(?:</dev/rga>)?', line)
        if opened:
            self.fds.add(int(opened.group(1)))
        closed = re.search(r'close\((\d+)</dev/rga(?:<[^>]*>)?>\)\s*=\s*0', line)
        if closed:
            self.fds.discard(int(closed.group(1)))
        begin = re.search(r'write\([^\n]*"W6_BEGIN:(\d+)\\n"', line)
        end = re.search(r'write\([^\n]*"W6_END:(\d+):(\d+)\\n"', line)
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
    check = TraceCheck()
    tails = {p: Tail(p) for p in (trace, appout, apperr, kern)}
    reason = 'internal error'
    target = None; logger = None
    with kern.open('xb', buffering=0) as kl, kernerr.open('xb', buffering=0) as ke, \
         appout.open('xb', buffering=0) as ao, apperr.open('xb', buffering=0) as ae:
        try:
            check_resources(root, temperature, max_temp, reserve_bytes, reserve_inodes)
            if find_d_state(): raise ValueError('D-state process before trial')
            logger = subprocess.Popen(['stdbuf', '-oL', 'dmesg', '-x', '-w'],
                                      stdout=kl, stderr=ke, stdin=subprocess.DEVNULL)
            time.sleep(.2)
            if logger.poll() is not None:
                raise ValueError('kernel log monitor could not start')
            cmd = ['strace', '-f', '-yy', '-s', '160', '-e', 'trace=write,ioctl,openat,close',
                   '-e', 'raw=ioctl', '-o', str(trace), 'stdbuf', '-oL', '-eL', str(binary),
                   mode, '30', '/dev/video5']
            env = os.environ.copy(); env['RUNS'] = '1'
            target = subprocess.Popen(cmd, cwd=str(out), env=env, stdout=ao, stderr=ae,
                                      stdin=subprocess.DEVNULL, start_new_session=True)
            deadline = time.monotonic() + duration
            next_d_check = 0
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
                    problem = kernel_problem(line, since)
                    if problem: raise ValueError('new kernel error: ' + problem)
                for path in (appout, apperr):
                    for line in tails[path].lines():
                        problem = app_problem(line)
                        if problem: raise ValueError('application error: ' + problem)
                for line in tails[trace].lines():
                    check.line(line)
                if target.poll() is not None:
                    for line in tails[trace].lines(final=True): check.line(line)
                    for path in (appout, apperr):
                        for line in tails[path].lines(final=True):
                            problem = app_problem(line)
                            if problem: raise ValueError('application error: ' + problem)
                    if target.returncode != 0: raise ValueError('test process exit %d' % target.returncode)
                    check.finish(30)
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
            problem = kernel_problem(line, since)
            if problem and reason.startswith('complete;'):
                reason = 'new kernel error: ' + problem
    with (out / 'monitor.txt').open('x') as f:
        f.write('mode=%s\nboot_time_start=%.6f\nobserved_rga_frames=%d\nreason=%s\n' %
                (mode, since, check.completed, reason))
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
