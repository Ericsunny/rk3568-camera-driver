#!/usr/bin/env python3
"""Run the real launcher in an isolated tree with a file-triggered event."""
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import time
from run_probe import collect, kernel_alert

base = Path(__file__).resolve().parent
out = base / 'launcher-final-imx'
out.mkdir(mode=0o700)
run_dir = out / 'service'
trace = out / 'rga.trace'
env = dict(os.environ, ENCODER_INPUT='dmabuf', RTSP_PORT_OVERRIDE='18760', MAX_EVENTS='1')
command = ['strace', '-D', '-f', '-yy', '-e', 'trace=ioctl', '-P', '/dev/rga', '-o', str(trace),
           'sh', str(base / 'candidate-v2/tools/run_camera_demo.sh'), 'imx415-isp', 'manual', str(run_dir)]
(out / 'plan.json').write_text(json.dumps({'argv': command, 'env_overrides': {
    'ENCODER_INPUT': 'dmabuf', 'RTSP_PORT_OVERRIDE': '18760', 'MAX_EVENTS': '1'},
    'pass': 'real launcher selects dmabuf, trigger file creates clean event, MP4 delivery complete, '
            '0 exit, no residual process, expected per-process RGA version probes only',
    'timeout_s': 90}, indent=2) + '\n')
if collect(['systemctl','is-active','rkaiq_3A'])['rc'] or collect(['fuser','/dev/video5'])['rc'] != 1:
    raise SystemExit('precheck failed')
kfd = os.open('/dev/kmsg', os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
os.lseek(kfd, 0, os.SEEK_END)
proc = None
reason = None
probes = 0
triggered = False
start = time.monotonic()
d_since = {}
try:
    with (out / 'run.log').open('wb') as log, (out / 'kernel-new.log').open('w') as kernel:
        proc = subprocess.Popen(command, cwd=str(base), env=env, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        while proc.poll() is None:
            while select.select([kfd], [], [], 0)[0]:
                try:
                    raw = os.read(kfd, 65536)
                except BlockingIOError:
                    break
                alert, text = kernel_alert(raw)
                kernel.write(text + '\n'); kernel.flush()
                message = text.partition(';')[2].splitlines()[0]
                if message == 'rga2: unknown ioctl cmd!':
                    probes += 1
                    alert = probes > 4
                if alert:
                    raise RuntimeError('kernel alert: ' + text.strip())
            now = time.monotonic()
            ids = [proc.pid]
            pid_file = run_dir / 'producer.pid'
            if pid_file.exists():
                try:
                    ids.append(int(pid_file.read_text()))
                except ValueError:
                    raise RuntimeError('invalid recorder PID')
            seen = set()
            for pid in ids:
                try:
                    tasks = list(Path('/proc/%d/task' % pid).iterdir())
                except FileNotFoundError:
                    continue
                for task in tasks:
                    try:
                        status = (task / 'status').read_text()
                    except FileNotFoundError:
                        continue
                    if re.search(r'^State:\s+D\s', status, re.M):
                        seen.add(task.name); d_since.setdefault(task.name, now)
                        if now - d_since[task.name] >= 3:
                            raise RuntimeError('thread remained D for 3 seconds')
            d_since = {key: value for key, value in d_since.items() if key in seen}
            if trace.exists():
                pending_pids = re.findall(r'^(\d+) ioctl\([^\n]*_IOC\(_IOC_READ, 0x72, 0x1, 0x1c\)[^\n]*= -1 EINVAL',
                                          trace.read_text(errors='replace'), re.M)
                if any(pending_pids.count(pid) > 1 for pid in set(pending_pids)):
                    raise RuntimeError('a process repeated the RGA version probe')
            if not triggered and now - start >= 6 and '预录就绪' in (out / 'run.log').read_text(errors='replace'):
                (run_dir / 'trigger').touch(exist_ok=False)
                triggered = True
            if now - start > 90:
                raise RuntimeError('launcher timeout')
            time.sleep(.05)
        rc = proc.returncode
except (OSError, ValueError, RuntimeError) as exc:
    reason = str(exc)
finally:
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            reason = (reason or '') + '; supervisor still alive'
    os.close(kfd)
trace_text = trace.read_text(errors='replace') if trace.exists() else ''
pids = re.findall(r'^(\d+) ioctl\([^\n]*_IOC\(_IOC_READ, 0x72, 0x1, 0x1c\)[^\n]*= -1 EINVAL', trace_text, re.M)
counts = {pid: pids.count(pid) for pid in set(pids)}
delivery = list(run_dir.glob('event*.delivery.json'))
payload = json.loads(delivery[0].read_text()) if len(delivery) == 1 else {}
args = (run_dir / 'argv.txt').read_text(errors='replace').splitlines() if (run_dir / 'argv.txt').exists() else []
stream_log = (out / 'run.log').read_text(errors='replace')
passed = (proc and proc.poll() == 0 and reason is None and triggered and
          len(delivery) == 1 and payload.get('status') == 'complete' and
          '--encoder-input' in args and args[args.index('--encoder-input') + 1] == 'dmabuf' and
          'mode=dmabuf copy_bytes=0' in stream_log and probes == len(pids) and
          all(n == 1 for n in counts.values()) and collect(['fuser','/dev/video5'])['rc'] == 1 and
          collect(['systemctl','is-active','rkaiq_3A'])['rc'] == 0)
result = {'rc': proc.poll() if proc else None, 'reason': reason, 'triggered': triggered,
          'version_probes': probes, 'trace_pids': counts, 'delivery': payload.get('status'), 'pass': bool(passed)}
(out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
sys.exit(0 if passed else 1)
