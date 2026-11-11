#!/usr/bin/env python3
"""Run two independent camera services with separate ports and output paths."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def run_pair(mode, run_dir, wrapper, imx_port, ov_port, shutdown_timeout=150):
    if mode not in ('manual', 'detect'):
        raise ValueError('mode must be manual or detect')
    if not run_dir.is_absolute():
        raise ValueError('run directory must be absolute')
    if not (1 <= imx_port <= 65535 and 1 <= ov_port <= 65535) or imx_port == ov_port:
        raise ValueError('RTSP ports must be distinct and between 1 and 65535')
    run_dir.mkdir(mode=0o700)  # Never reuse or overwrite a prior run.
    children = {}
    streams = []
    stopping = False
    stop_time = None
    requested_signal = None
    error = None

    def request_stop(signum, _frame):
        nonlocal requested_signal
        requested_signal = signum

    old_handlers = {sig: signal.signal(sig, request_stop)
                    for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        for name, profile, port in (('imx415', 'imx415-isp', imx_port),
                                    ('ov8858', 'ov8858-isp', ov_port)):
            if requested_signal is not None:
                break
            output = (run_dir / (name + '.log')).open('x', encoding='utf-8')
            streams.append(output)
            env = dict(os.environ, RTSP_PORT_OVERRIDE=str(port))
            children[name] = subprocess.Popen(
                ['sh', str(wrapper), profile, mode, str(run_dir / name)],
                stdout=output, stderr=subprocess.STDOUT, env=env)
        while True:
            alive = [p for p in children.values() if p.poll() is None]
            # An unexpected exit in either service ends the paired run. This
            # avoids silently presenting a single camera as a working pair.
            if not stopping and (requested_signal is not None or len(children) < 2 or len(alive) < 2):
                stopping = True
                stop_time = time.monotonic()
                for child in alive:
                    try:
                        child.terminate()
                    except ProcessLookupError:
                        pass
            if not alive:
                break
            if stopping and time.monotonic() - stop_time > shutdown_timeout:
                error = 'shutdown timeout; inspect remaining processes and devices'
                break
            time.sleep(0.2)
    except (OSError, ValueError) as exc:
        error = str(exc)
        for child in children.values():
            if child.poll() is None:
                child.terminate()
        for child in children.values():
            try:
                child.wait(timeout=shutdown_timeout)
            except subprocess.TimeoutExpired:
                error += '; child shutdown timeout'
    finally:
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
        for stream in streams:
            stream.close()

    exits = {name: {'pid': proc.pid, 'returncode': proc.poll()} for name, proc in children.items()}
    # A normal user-requested stop is successful only if both children report
    # clean shutdown. A premature service exit is a paired-run failure.
    ok = (requested_signal is not None and len(children) == 2 and error is None
          and all(item['returncode'] == 0 for item in exits.values()))
    result = {'status': 'stopped' if ok else 'failed', 'signal': requested_signal,
              'mode': mode, 'ports': {'imx415': imx_port, 'ov8858': ov_port},
              'children': exits, 'error': error}
    (run_dir / 'status.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('manual', 'detect'))
    parser.add_argument('run_dir', type=Path, help='new absolute directory')
    parser.add_argument('--imx-port', type=int, default=8554)
    parser.add_argument('--ov-port', type=int, default=8555)
    args = parser.parse_args()
    wrapper = Path(__file__).resolve().with_name('run_camera_demo.sh')
    try:
        return run_pair(args.mode, args.run_dir, wrapper, args.imx_port, args.ov_port)
    except (OSError, ValueError) as exc:
        print('dual camera: ' + str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
