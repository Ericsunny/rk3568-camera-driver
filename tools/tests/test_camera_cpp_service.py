#!/usr/bin/env python3
"""Host tests of the C++ service using synthetic video and process substitutes."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def build(target, output):
    subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'tools' / (target + '.cpp')), '-o', str(output)], check=True)


def executable(path, text):
    path.write_text('#!/usr/bin/env python3\n' + text, encoding='utf-8')
    path.chmod(0o755)


def main():
    with tempfile.TemporaryDirectory(prefix='camera cpp test ') as tmp:
        tmp = Path(tmp)
        for name in ('camera_demo', 'camera_service', 'camera_delivery'):
            build(name, tmp / name)
        video = tmp / 'sample.h264'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=160x120:rate=10:duration=1', '-c:v', 'libx264',
                        '-bf', '0', '-f', 'h264', str(video)], check=True)

        def prepare(name, overrides=None):
            run = tmp / name
            run.mkdir()
            shutil.copyfile(video, run / 'event-01.h264')
            data = dict(write_error=False, bytes=video.stat().st_size,
                        first_ts_ms=1000, last_ts_ms=1900,
                        event_id=1, segment_index=0, close_reason='target_left')
            data.update(overrides or {})
            (run / 'event-01.json').write_text(json.dumps(data))
            return run

        def finalize(run):
            return subprocess.run([str(tmp / 'camera_delivery'), str(run / 'event-01.json')],
                                  capture_output=True, text=True, timeout=30)

        run = prepare('success')
        result = finalize(run)
        assert result.returncode == 0, (result.stdout, result.stderr)
        info = json.loads((run / 'event-01.delivery.json').read_text())
        assert info['status'] == 'complete' and info['video_frames'] == 10
        assert info['playback_fps'] == '10' and (run / 'event-01.mp4').exists()
        original = (run / 'event-01.mp4').read_bytes()
        assert finalize(run).returncode != 0 and (run / 'event-01.mp4').read_bytes() == original

        for name, changes in [('write-failure', {'write_error': True}),
                              ('bad-size', {'bytes': 1}), ('bad-time', {'last_ts_ms': 1000}),
                              ('missing-csv', {'schema': 2, 'timestamp_source': 'v4l2-monotonic'})]:
            run = prepare(name, changes)
            assert finalize(run).returncode != 0
            assert json.loads((run / 'event-01.delivery.json').read_text())['status'] == 'failed'
            assert not (run / 'event-01.mp4').exists()
        run = prepare('schema2', {'schema': 2, 'timestamp_source': 'v4l2-monotonic'})
        packet_sizes = [video.stat().st_size - 9] + [1] * 9
        rows = ['frame_seq,packet_index,media_ms,capture_ms,bytes,decodable_start']
        rows += [f'{i},0,{1000+i*100},{1000+i*100},{size},{int(i == 0)}'
                 for i, size in enumerate(packet_sizes)]
        (run / 'event-01.packets.csv').write_text('\n'.join(rows) + '\n')
        result = finalize(run)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert 'packet_log_sha256' in json.loads((run / 'event-01.delivery.json').read_text())
        run = prepare('bad-packets', {'schema': 2, 'timestamp_source': 'v4l2-monotonic'})
        (run / 'event-01.packets.csv').write_text('\n'.join(rows[:-1]) + '\n')
        assert finalize(run).returncode != 0
        assert not (run / 'event-01.mp4').exists()
        run = prepare('bad-json')
        (run / 'event-01.json').write_text('{"write_error": false,')
        assert finalize(run).returncode != 0
        assert json.loads((run / 'event-01.delivery.json').read_text())['status'] == 'failed'
        run = prepare('non-utf8-error')
        noisybin = tmp / 'noisybin'
        noisybin.mkdir()
        executable(noisybin / 'ffprobe', "import sys\nsys.stderr.buffer.write(b'\\xff')\nsys.exit(1)\n")
        result = subprocess.run([str(tmp / 'camera_delivery'), str(run / 'event-01.json')],
                                env=dict(os.environ, PATH=str(noisybin) + ':' + os.environ['PATH']),
                                capture_output=True, timeout=20)
        assert result.returncode != 0
        assert json.loads((run / 'event-01.delivery.json').read_text())['status'] == 'failed'

        producer = tmp / 'producer'
        executable(producer, '''
import os, pathlib, shutil, signal, sys, time
run = pathlib.Path(sys.argv[1])
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
shutil.copyfile(sys.argv[2], run / 'event-01.h264')
(run / 'event-01.json').write_text(sys.argv[3])
(run / 'ready').touch()
while True: time.sleep(.05)
''')
        run = tmp / 'service'
        run.mkdir()
        metadata = json.dumps(dict(write_error=False, bytes=video.stat().st_size,
                                   first_ts_ms=1000, last_ts_ms=1900))
        proc = subprocess.Popen([str(tmp / 'camera_service'), str(run), '--',
                                 str(producer), str(run), str(video), metadata],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 15
            while not (run / 'event-01.delivery.json').exists() and time.monotonic() < deadline:
                time.sleep(.05)
            assert (run / 'event-01.delivery.json').exists()
            proc.send_signal(signal.SIGTERM)
            out, err = proc.communicate(timeout=10)
            assert proc.returncode == 0, (out, err)
            assert json.loads((run / 'event-01.delivery.json').read_text())['status'] == 'complete'
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

        run = tmp / 'orphan'
        run.mkdir()
        (run / 'event-01.h264').write_bytes(video.read_bytes())
        result = subprocess.run([str(tmp / 'camera_service'), str(run), '--', '/bin/true'],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0
        run = tmp / 'queue-full'
        run.mkdir()
        for i in range(33):
            (run / f'event-{i:02}.json').write_text('{}')
        result = subprocess.run([str(tmp / 'camera_service'), str(run), '--', '/bin/true'],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and 'capacity exceeded' in result.stderr

        # Exercise config reading and preflight with substitutes, including paths with spaces.
        root = tmp / 'mock root'
        root.mkdir()
        shutil.copytree(ROOT / 'config', root / 'config')
        executable(root / 'resolve_camera', "print('/dev/null')\n")
        executable(root / 'evrec', "print('--encoder-input --npu-input --detect-every')\n")
        executable(root / 'camera_service', "import pathlib,sys\npathlib.Path(sys.argv[1], 'service-argv').write_text('\\n'.join(sys.argv[2:]))\n")
        executable(root / 'camera_delivery', "pass\n")
        fakebin = tmp / 'bin'
        fakebin.mkdir()
        executable(fakebin / 'pgrep', "print('12345')\n")
        executable(fakebin / 'fuser', "import sys\nsys.exit(1)\n")
        executable(fakebin / 'ss', "pass\n")
        env = dict(os.environ, CAMERA_ROOT=str(root), PATH=str(fakebin) + ':' + os.environ['PATH'])
        run = tmp / 'manual'
        result = subprocess.run([str(tmp / 'camera_demo'), 'imx415-isp', 'manual', str(run)],
                                env=env, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, (result.stdout, result.stderr)
        args = (run / 'argv.txt').read_text().splitlines()
        assert '--encoder-input' in args and '--trigger' in args and '--detect' not in args
        assert (run / 'service-argv').exists()
        assert subprocess.run([str(tmp / 'camera_demo'), 'imx415-isp', 'manual', str(run)],
                              env=env, capture_output=True).returncode != 0
        import hashlib
        with (root / 'config/final-demo.env').open('a') as stream:
            stream.write(f'\nMODEL=/dev/null\nMODEL_SHA256={hashlib.sha256(b"").hexdigest()}\n')
        run = tmp / 'detect'
        result = subprocess.run([str(tmp / 'camera_demo'), 'ov8858-isp', 'detect', str(run)],
                                env=env, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, (result.stdout, result.stderr)
        args = (run / 'argv.txt').read_text().splitlines()
        assert '--encoder-input' in args and '--npu-input' in args and '--detect' in args
        executable(fakebin / 'fuser', "import sys\nsys.exit(0)\n")
        blocked = tmp / 'blocked'
        assert subprocess.run([str(tmp / 'camera_demo'), 'imx415-isp', 'manual', str(blocked)],
                              env=env, capture_output=True).returncode != 0
        assert not blocked.exists()
        print('PASS C++ delivery, error propagation, supervisor, config and preflight')


if __name__ == '__main__':
    main()
