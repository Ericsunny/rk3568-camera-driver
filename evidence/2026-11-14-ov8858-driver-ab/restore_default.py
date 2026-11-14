#!/usr/bin/env python3
"""Reboot once to the unchanged default Image and DTB, recording serial output."""
from pathlib import Path
import subprocess
import time
import serial

SSH = r'C:\Windows\System32\OpenSSH\ssh.exe'
LOG = Path(r'D:\vm\board-test-logs\ov8858-driver-ab-20261114-restore.raw')
with LOG.open('xb') as log:
    port = serial.Serial()
    port.port, port.baudrate = 'COM8', 1500000
    port.timeout, port.write_timeout = .1, 2
    port.open()
    port.dtr = False
    port.rts = False
    try:
        subprocess.Popen([SSH, '-o', 'BatchMode=yes', 'rkboard', 'systemctl reboot'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        transcript = bytearray()
        deadline = time.monotonic() + 75
        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                log.write(chunk)
                log.flush()
                transcript.extend(chunk)
                if b'Kernel panic' in transcript:
                    raise RuntimeError('default boot kernel panic; inspect serial log')
                if b'yckt login:' in transcript:
                    print('Default boot reached login; verify state over SSH', flush=True)
                    break
        else:
            raise RuntimeError('default boot login prompt not observed')
    finally:
        port.close()
