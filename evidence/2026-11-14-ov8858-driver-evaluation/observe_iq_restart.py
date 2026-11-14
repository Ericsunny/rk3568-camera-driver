#!/usr/bin/env python3
"""Observe IQ-directory opens during an idle 3A restart; inotify has no PID."""
import ctypes
import json
import os
import select
import struct
import subprocess
import sys
import time

lib = ctypes.CDLL(None, use_errno=True)
lib.inotify_init1.argtypes = [ctypes.c_int]
lib.inotify_init1.restype = ctypes.c_int
lib.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
lib.inotify_add_watch.restype = ctypes.c_int
IN_OPEN = 0x20


def pids():
    p = subprocess.run(['pgrep', '-f', '^/usr/bin/rkaiq_3A_server([[:space:]]|$)'],
                       text=True, stdout=subprocess.PIPE, check=False)
    return [int(x) for x in p.stdout.split()]


for dev in ('/dev/video5', '/dev/video14'):
    if subprocess.run(['fuser', dev], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode == 0:
        sys.exit('capture node busy: ' + dev)
old = pids()
if len(old) != 1:
    sys.exit('expected exactly one 3A process before restart')
fd = lib.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
if fd < 0:
    raise OSError(ctypes.get_errno(), 'inotify_init1')
wd = lib.inotify_add_watch(fd, b'/etc/iqfiles', IN_OPEN)
if wd < 0:
    raise OSError(ctypes.get_errno(), 'inotify_add_watch')
start = time.monotonic()
events = []
try:
    restart = subprocess.run(['systemctl', 'restart', 'rkaiq_3A.service'],
                             text=True, capture_output=True, timeout=30)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], max(0, min(.3, deadline - time.monotonic())))[0]:
            continue
        data = os.read(fd, 65536)
        pos = 0
        while pos + 16 <= len(data):
            watch, mask, cookie, length = struct.unpack_from('iIII', data, pos)
            name = data[pos + 16:pos + 16 + length].split(b'\0', 1)[0].decode('utf-8', 'replace')
            pos += 16 + length
            if mask & IN_OPEN:
                events.append({'name': name, 'seconds_after_start': round(time.monotonic()-start, 3)})
    new = pids()
    active = subprocess.run(['systemctl', 'is-active', 'rkaiq_3A.service'],
                            text=True, stdout=subprocess.PIPE, check=False)
    result = {'old_pids': old, 'new_pids': new, 'restart_rc': restart.returncode,
              'restart_stderr': restart.stderr, 'service_status': active.stdout.strip(),
              'events': events, 'note': 'inotify does not identify which process opened a file'}
    print(json.dumps(result, indent=2, ensure_ascii=False))
    if restart.returncode or len(new) != 1 or active.stdout.strip() != 'active':
        sys.exit(1)
finally:
    os.close(fd)
