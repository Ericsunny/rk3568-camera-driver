#!/usr/bin/env python3
"""Bounded IMX415/manual C++ service baseline. Run on the board in a private dir."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import time

ERROR = re.compile(r'crc errors|err[12]:|kernel panic|\boops\b|\bBUG:|'
                   r'timed out|\btimeout\b|hung task|lockup|unable to handle', re.I)
DECODE_ERROR = re.compile(r'error|invalid|corrupt|conceal|missing|non-existing|failed', re.I)
CSI_NOTICE = re.compile(r'rockchip-mipi-csi2 [^:]+: stream '
                        r'(?:(?:on|off), src_sd: [^,]+, sd_name:rockchip-csi2-dphy\d+|ON|OFF)')


def collect(argv, timeout=15):
    p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return {'argv': argv, 'rc': p.returncode,
            'stdout': p.stdout.decode('utf-8', 'replace'),
            'stderr': p.stderr.decode('utf-8', 'replace')}


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def parse_format(text):
    size = re.search(r'Width/Height\s*:\s*(\d+)/(\d+)', text)
    fourcc = re.search(r"Pixel Format\s*:\s*'([^']+)'", text)
    if not size or not fourcc:
        raise RuntimeError('cannot parse capture format; will not start')
    return {'width': int(size[1]), 'height': int(size[2]), 'fourcc': fourcc[1]}


def progress_frames(text):
    values = re.findall(r'^frame=\s*(\d+)\s*$', text, re.M)
    return int(values[-1]) if values else 0


def assess(result):
    reasons = []
    if result.get('stop'):
        reasons.append(result['stop'])
    if result.get('cleanup_error'):
        reasons.append(result['cleanup_error'])
    if result.get('frame_metrics_requested') and not result.get('frame_analysis'):
        reasons.append('requested frame observations did not validate')
    if result.get('service_rc') != 0:
        reasons.append('service did not exit successfully')
    if not result.get('triggered_with_two_clients'):
        reasons.append('trigger did not overlap two PLAY clients')
    clients = result.get('clients', [])
    if len(clients) != 2 or any(c['rc'] != 0 or c['frames'] < 90 or c['errors'] for c in clients):
        reasons.append('two strict software decoders did not complete 90 frames')
    if result.get('delivery_status') != 'complete':
        reasons.append('event delivery is not complete')
    decodes = result.get('software_decode', [])
    if len(decodes) != 2 or any(c['rc'] or c['stderr'].strip() for c in decodes):
        reasons.append('source and MP4 software decode did not both pass')
    counts = [c.get('frames', 0) for c in decodes]
    if len(counts) != 2 or counts[0] <= 0 or counts[0] != counts[1] or counts[0] != result.get('delivery_frames'):
        reasons.append('software frame counts do not match each other and delivery metadata')
    after = result.get('after', {})
    if after.get('3a', {}).get('rc') != 0 or after.get('busy', {}).get('rc') != 1:
        reasons.append('3A or capture cleanup check failed')
    if not result.get('format_restored') or result.get('service_alive'):
        reasons.append('format restoration or process cleanup failed')
    if result.get('port_after', {}).get('stdout', '').strip() or result.get('port_after', {}).get('rc') != 0:
        reasons.append('test port cleanup failed')
    return reasons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--port', type=int, default=18554)
    parser.add_argument('--encoder-input', choices=('copy', 'dmabuf'), default='dmabuf')
    parser.add_argument('--frame-metrics', action='store_true')
    a = parser.parse_args()
    source, out = a.source.resolve(), a.out.absolute()
    if not out.is_absolute() or not 1024 <= a.port <= 65535:
        parser.error('absolute new directory and unprivileged port required')
    out.mkdir(mode=0o700)
    run_dir = out / 'run'
    result = {'schema': 1, 'stop': None, 'service_rc': None, 'clients': [],
              'frame_metrics_requested': a.frame_metrics,
              'triggered_with_two_clients': False, 'format_restored': False,
              'software_decode': [], 'scope': 'manual IMX415; two loopback software decoders; no display latency'}
    service, clients, streams, kfd = None, [], [], -1
    device, before_format, original = None, None, None
    started = time.monotonic()
    kernel_rows = []
    save(out / 'plan.json', {'timeout_s': 60, 'D_limit_s': 3, 'source': str(source),
                            'encoder_input': a.encoder_input, 'port': a.port, 'detect': False,
                            'frame_metrics': a.frame_metrics,
                            'client_frames': 90, 'trigger': 'preroll ready AND both PLAY clients online'})
    try:
        resolved = collect([str(source / 'resolve_camera'), 'imx415-isp'])
        if resolved['rc'] or not resolved['stdout'].strip().startswith('/dev/video'):
            raise RuntimeError('IMX415 media graph resolution failed')
        device = resolved['stdout'].strip()
        before_format = collect(['v4l2-ctl', '-d', device, '--get-fmt-video'])
        if before_format['rc']:
            raise RuntimeError('cannot read original format')
        original = parse_format(before_format['stdout'])
        before = {'resolve': resolved, 'format': before_format,
                  '3a': collect(['systemctl', 'is-active', 'rkaiq_3A']),
                  'busy': collect(['fuser', device]), 'clock': collect(['date', '-Iseconds']),
                  'uname': collect(['uname', '-a']),
                  'port': collect(['ss', '-H', '-ltn', 'sport = :%d' % a.port])}
        save(out / 'before.json', before)
        if before['3a']['rc'] or before['busy']['rc'] != 1 or before['port']['rc'] or before['port']['stdout'].strip():
            raise RuntimeError('shared board preflight failed')
        kfd = os.open('/dev/kmsg', os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        os.lseek(kfd, 0, os.SEEK_END)

        def drain():
            while select.select([kfd], [], [], 0)[0]:
                raw = os.read(kfd, 65536).decode('utf-8', 'replace')
                kernel_rows.append(raw)
                (out / 'kernel-new.log').write_text('\n'.join(kernel_rows), encoding='utf-8')
                head, sep, msg = raw.partition(';')
                if not sep:
                    raise RuntimeError('invalid kernel record')
                first = msg.splitlines()[0]
                known = first == 'rga2: unknown ioctl cmd!' or CSI_NOTICE.fullmatch(first)
                if ERROR.search(msg) or ((int(head.split(',')[0]) & 7) <= 3 and not known):
                    raise RuntimeError('kernel alert: ' + msg.strip())

        env = dict(os.environ, CAMERA_ROOT=str(source), RTSP_PORT_OVERRIDE=str(a.port),
                   ENCODER_INPUT=a.encoder_input, MAX_EVENTS='0')
        env['FRAME_METRICS'] = str(run_dir / 'frame-metrics.csv') if a.frame_metrics else ''
        log = (out / 'service.log').open('wb')
        streams.append(log)
        argv = ['stdbuf', '-oL', '-eL', str(source / 'camera_demo'), 'imx415-isp', 'manual', str(run_dir)]
        save(out / 'command.json', {'argv': argv, 'environment': {k: env[k] for k in
             ('CAMERA_ROOT', 'RTSP_PORT_OVERRIDE', 'ENCODER_INPUT', 'MAX_EVENTS', 'FRAME_METRICS')}})
        service = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        d_since = {}
        stopping = False
        while True:
            drain()
            now = time.monotonic()
            if now - started > 60:
                raise RuntimeError('baseline deadline exceeded')
            if service.poll() is not None:
                break
            text = (out / 'service.log').read_text(errors='replace')
            pidfile = run_dir / 'producer.pid'
            if pidfile.exists():
                pid = int(pidfile.read_text().strip())
                for task in Path('/proc/%d/task' % pid).glob('*'):
                    try:
                        state = (task / 'stat').read_text().rsplit(')', 1)[1].split()[0]
                    except (FileNotFoundError, ProcessLookupError):
                        continue
                    if state == 'D':
                        d_since.setdefault(task.name, now)
                        if now - d_since[task.name] >= 3:
                            raise RuntimeError('recorder thread remained D for 3 seconds')
                    else:
                        d_since.pop(task.name, None)
            if not clients and '>>> RTSP 监听 :%d ' % a.port in text:
                for i in range(2):
                    err = (out / ('client-%d.stderr' % i)).open('wb')
                    prog = (out / ('client-%d.progress' % i)).open('wb')
                    streams.extend([err, prog])
                    cmd = ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'warning',
                           '-nostats', '-progress', 'pipe:1', '-rtsp_transport', 'tcp',
                           '-hwaccel', 'none', '-c:v', 'h264', '-xerror', '-err_detect', 'explode',
                           '-i', 'rtsp://127.0.0.1:%d/live' % a.port,
                           '-map', '0:v:0', '-an', '-frames:v', '90', '-f', 'null', '-']
                    save(out / ('client-%d.command.json' % i), cmd)
                    clients.append(subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=prog, stderr=err))
            online = all('rtsp c%d: 客户端开始播放' % i in text and
                         'rtsp c%d: 连接关闭' % i not in text for i in range(2))
            if not result['triggered_with_two_clients'] and '预录就绪' in text and online:
                (run_dir / 'trigger').touch()
                result['triggered_with_two_clients'] = True
                result['trigger_monotonic_s'] = now - started
            reports = list(run_dir.glob('event*.delivery.json'))
            if reports:
                report = json.loads(reports[0].read_text())
                result['delivery_status'] = report.get('status')
                result['delivery_frames'] = report.get('video_frames')
                if report.get('status') != 'complete':
                    raise RuntimeError('delivery failed: ' + report.get('error', 'unknown'))
            if clients and all(c.poll() is not None for c in clients) and reports and not stopping:
                service.terminate()
                stopping = True
            time.sleep(.05)
        result['service_rc'] = service.returncode
        for i, child in enumerate(clients):
            child.wait(timeout=5)
            err = (out / ('client-%d.stderr' % i)).read_text(errors='replace')
            prog = (out / ('client-%d.progress' % i)).read_text(errors='replace')
            result['clients'].append({'rc': child.returncode, 'frames': progress_frames(prog),
                                      'errors': [s for s in err.splitlines() if DECODE_ERROR.search(s)]})
        for suffix in ('h264', 'mp4'):
            files = list(run_dir.glob('event*.' + suffix))
            if len(files) != 1:
                raise RuntimeError('expected exactly one ' + suffix + ' event')
            decoded = collect(['ffmpeg', '-nostdin', '-v', 'error', '-nostats', '-progress', 'pipe:1',
                 '-hwaccel', 'none', '-c:v', 'h264', '-xerror', '-err_detect', 'explode',
                 '-i', str(files[0]), '-map', '0:v:0', '-f', 'null', '-'], timeout=20)
            decoded['frames'] = progress_frames(decoded['stdout'])
            result['software_decode'].append(decoded)
        text = (out / 'service.log').read_text(errors='replace')
        usage = re.search(r'frame-metrics: enabled=(\d+) seen=(\d+) stored=(\d+) completed=(\d+) '
                          r'cpu_user_ms=([0-9.]+) cpu_system_ms=([0-9.]+)', text)
        if usage:
            result['recorder_usage'] = dict(zip(('enabled', 'seen', 'stored', 'completed'),
                                                map(int, usage.groups()[:4])))
            result['recorder_usage'].update(user_ms=float(usage[5]), system_ms=float(usage[6]),
                scope='recorder process CPU sampled after final cleanup; includes startup and metrics output; excludes supervisor, remux, decoders and 3A')
        if a.frame_metrics:
            check = collect(['python3', str(source / 'tools/analyze_frame_metrics.py'),
                             str(run_dir / 'frame-metrics.csv'), '--warmup', '30',
                             '-o', str(out / 'frame-summary.json')])
            result['frame_check'] = check
            if check['rc']:
                raise RuntimeError('frame observations rejected: ' + check['stderr'])
            result['frame_analysis'] = json.loads(check['stdout'])
        drain()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        result['stop'] = str(exc)
    finally:
        for child in clients:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    result['stop'] = result['stop'] or 'test decoder did not exit'
        if service and service.poll() is None:
            service.terminate()
            try:
                service.wait(timeout=10)
            except subprocess.TimeoutExpired:
                result['stop'] = result['stop'] or 'service did not exit; inspect owned process group'
        result['service_alive'] = bool(service and service.poll() is None)
        if service:
            result['service_rc'] = service.poll()
        try:
            if device:
                result['after'] = {'3a': collect(['systemctl', 'is-active', 'rkaiq_3A']),
                                   'busy': collect(['fuser', device])}
                if original and not result['service_alive'] and result['after']['busy']['rc'] == 1:
                    current = collect(['v4l2-ctl', '-d', device, '--get-fmt-video'])
                    if parse_format(current['stdout']) != original:
                        spec = 'width={width},height={height},pixelformat={fourcc}'.format(**original)
                        result['restore'] = collect(['v4l2-ctl', '-d', device, '--set-fmt-video=' + spec])
                    final_fmt = collect(['v4l2-ctl', '-d', device, '--get-fmt-video'])
                    result['format_after'] = final_fmt
                    result['format_restored'] = final_fmt['rc'] == 0 and final_fmt['stdout'] == before_format['stdout']
            result['port_after'] = collect(['ss', '-H', '-ltn', 'sport = :%d' % a.port])
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            result['cleanup_error'] = str(exc)
        if kfd >= 0:
            os.close(kfd)
        for stream in streams:
            stream.close()
        result['elapsed_monotonic_s'] = round(time.monotonic() - started, 3)
        result['known_rga_probe_lines'] = sum('rga2: unknown ioctl cmd!' in s for s in kernel_rows)
        result['failure_reasons'] = assess(result)
        result['pass'] = not result['failure_reasons']
        save(out / 'result.json', result)
        print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
