#!/usr/bin/env python3
"""Linux-only launcher checks with fake daemon/device/program boundaries."""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix='camera demo ') as name:
        root = Path(name)
        for part in ['tools','config','mock-bin']:
            (root/part).mkdir()
        script = root/'tools/run_camera_demo.sh'
        script.write_bytes((ROOT/'tools/run_camera_demo.sh').read_bytes())
        config = {}
        for line in (ROOT/'config/final-demo.env').read_text().splitlines():
            if line and not line.startswith('#'):
                key,value=line.split('=',1)
                config[key]=value
        model=root/'model.rknn'
        model.write_bytes(b'model test double')
        config.update(VIDEO='/dev/null',MODEL=str(model),
                      MODEL_SHA256=hashlib.sha256(model.read_bytes()).hexdigest())
        (root/'config/final-demo.env').write_text(''.join(
            key+'='+shlex.quote(value)+'\n' for key,value in config.items()))
        binary=root/'evrec'
        binary.write_text('#!/bin/sh\nif [ "${1:-}" = --help ]; then\n'
                          '  [ "${MOCK_SUPPORT_DMA:-1}" = 1 ] && echo --encoder-input\n'
                          '  exit 0\nfi\nprintf "%s\\n" "$@" > "$MOCK_ARGV"\n')
        binary.chmod(0o755)
        mocks={
            'pgrep':'[ "${MOCK_DAEMON:-1}" = 1 ] && echo 123\n',
            'fuser':'[ "${MOCK_BUSY:-0}" = 1 ]\n',
            'ss':'[ "${MOCK_PORT:-0}" = 1 ] && echo "LISTEN occupied"; exit 0\n',
            'ffmpeg':'exit 0\n',
            'ffprobe':'exit 0\n',
            'python3':'shift 3; exec "$@"\n',  # service script, run directory, --
        }
        for tool,body in mocks.items():
            p=root/'mock-bin'/tool
            p.write_text('#!/bin/sh\n'+body)
            p.chmod(0o755)
        base=dict(os.environ,PATH=str(root/'mock-bin')+':'+os.environ['PATH'],
                  MOCK_ARGV=str(root/'observed-args.txt'),MAX_EVENTS='1')
        def run(mode,path,**extra):
            return subprocess.run(['sh',str(script),mode,str(path)],
                                  env=dict(base,**extra),capture_output=True,text=True,timeout=5)
        for mode in ['manual','detect']:
            out=root/(mode+' run')
            r=run(mode,out)
            assert r.returncode==0,(r.stdout,r.stderr)
            args=(root/'observed-args.txt').read_text().splitlines()
            assert args[args.index('--rtsp-clients')+1]=='2'
            assert args[args.index('--max-events')+1]=='1'
            assert args[args.index('--fps')+1]=='15'
            assert args[args.index('--gop')+1]=='15'
            assert args[args.index('-o')+1]==str(out/'event')
            assert ('--detect' in args)==(mode=='detect')
            assert not (out/'trigger').exists()  # no implicit startup trigger
            if mode=='manual':
                assert args[args.index('--trigger')+1]==str(out/'trigger')
            assert run(mode,out).returncode!=0  # existing output is preserved
        out=root/'alternate-port'
        assert run('manual',out,RTSP_PORT_OVERRIDE='8555').returncode==0
        args=(root/'observed-args.txt').read_text().splitlines()
        assert args[args.index('--rtsp')+1]=='8555'
        out=root/'dmabuf'
        assert run('manual',out,ENCODER_INPUT='dmabuf').returncode==0
        args=(root/'observed-args.txt').read_text().splitlines()
        assert args[args.index('--encoder-input')+1]=='dmabuf'
        out=root/'unsupported-dmabuf'
        assert run('manual',out,ENCODER_INPUT='dmabuf',MOCK_SUPPORT_DMA='0').returncode!=0
        assert not out.exists()
        out=root/'invalid-input'
        assert run('manual',out,ENCODER_INPUT='other').returncode==2
        assert not out.exists()
        for port in ['0','65536','-1','abc','999999']:
            out=root/('bad-port-'+port)
            assert run('manual',out,RTSP_PORT_OVERRIDE=port).returncode==2
            assert not out.exists()
        for setting in [{'MOCK_DAEMON':'0'},{'MOCK_BUSY':'1'},{'MOCK_PORT':'1'}]:
            out=root/('reject-'+next(iter(setting)))
            assert run('manual',out,**setting).returncode!=0
            assert not out.exists()
        model.write_bytes(b'wrong model')
        assert run('detect',root/'wrong-model').returncode!=0
        assert run('invalid',root/'invalid').returncode==2
        print('PASS camera profile: modes, arguments, isolated outputs and startup rejection')


if __name__=='__main__':
    main()
