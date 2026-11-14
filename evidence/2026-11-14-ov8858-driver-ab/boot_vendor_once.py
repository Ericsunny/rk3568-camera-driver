#!/usr/bin/env python3
"""One serial boot of the vendor DTB; no persistent U-Boot or /boot changes."""
from pathlib import Path
import re
import subprocess
import time
import serial

SSH = r'C:\Windows\System32\OpenSSH\ssh.exe'
LOG = Path(r'D:\vm\board-test-logs\ov8858-driver-ab-20261114-serial.raw')
BASE = '/root/ov8858-driver-ab-20261114/'


def ssh(command):
    return subprocess.check_output([SSH, '-o', 'BatchMode=yes', 'rkboard', command],
                                   timeout=15).decode('utf-8').strip()


def main():
    cmdline = ssh('cat /proc/cmdline')
    if not re.fullmatch(r'[A-Za-z0-9_=.,/+: -]+', cmdline) or 'root=/dev/mmcblk0p3' not in cmdline:
        raise RuntimeError('unexpected boot arguments')
    checks = ssh('sha256sum /boot/Image-4.19.232 /boot/rk-kernel.dtb ' +
                 BASE + 'Image-baseline ' + BASE + 'self.dtb ' + BASE + 'vendor.dtb').splitlines()
    expected = ['40ec35eccd8e78ecacaee883257f2446d056614c50f649ae2d4988806bb07927',
                'eb38789325c307867f0d9d5b9b09c81123c48bc113cbd74af6d505877ccc6ce0',
                '40ec35eccd8e78ecacaee883257f2446d056614c50f649ae2d4988806bb07927',
                'eb38789325c307867f0d9d5b9b09c81123c48bc113cbd74af6d505877ccc6ce0',
                '4e7e817d425afbdaa92065154cc91044ae785fddc35ca788a9729725ce860f06']
    if [line.split()[0] for line in checks] != expected:
        raise RuntimeError('boot files differ from the frozen baseline')
    paths = [('/initrd-4.19.232', '${ramdisk_addr_r}', '0:2'),
             (BASE + 'Image-baseline', '${kernel_addr_r}', '0:3'),
             (BASE + 'vendor.dtb', '${fdt_addr_r}', '0:3')]
    sizes = [int(n) for n in ssh('stat -c %s /boot/initrd-4.19.232 ' +
                                  BASE + 'Image-baseline ' + BASE + 'vendor.dtb').splitlines()]
    if len(sizes) != 3 or min(sizes) <= 0:
        raise RuntimeError('missing boot inputs')
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open('xb') as log:
        port = serial.Serial()
        port.port, port.baudrate = 'COM8', 1500000
        port.timeout, port.write_timeout = .1, 2
        port.open()
        port.dtr = False
        port.rts = False
        transcript = bytearray()

        def read():
            chunk = port.read(4096)
            if chunk:
                log.write(chunk)
                log.flush()
                transcript.extend(chunk)

        def command(value, required=None):
            transcript.clear()
            port.write(value.encode('ascii') + b'\n')
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                read()
                if b'=>' in transcript:
                    output = transcript.decode('utf-8', 'replace')
                    if required and not re.search(required, output):
                        raise RuntimeError('unexpected U-Boot response to ' + value)
                    return output
            raise RuntimeError('U-Boot timeout: ' + value)

        try:
            print('Serial connected; rebooting for one temporary vendor-DTB boot', flush=True)
            subprocess.Popen([SSH, 'rkboard', 'systemctl reboot'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 35
            interrupting = False
            while time.monotonic() < deadline:
                read()
                if b'Jumping to U-Boot' in transcript:
                    interrupting = True
                if interrupting:
                    port.write(b'\x03')
                if b'=>' in transcript:
                    break
            else:
                raise RuntimeError('U-Boot prompt not observed')
            output = command('printenv kernel_addr_r ramdisk_addr_r fdt_addr_r')
            for key in ('kernel_addr_r', 'ramdisk_addr_r', 'fdt_addr_r'):
                if not re.search(r'\b' + key + r'=0x[0-9a-fA-F]+', output):
                    raise RuntimeError('missing U-Boot address ' + key)
            command("setenv bootargs '" + cmdline + "'")
            for (path, address, part), expected_size in zip(paths, sizes):
                command('load mmc ' + part + ' ' + address + ' ' + path,
                        r'\b' + str(expected_size) + r' bytes read\b')
            command('fdt addr ${fdt_addr_r}')
            command('fdt set /chosen bootargs')
            transcript.clear()
            port.write(b'booti ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}\n')
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                read()
                if b'Kernel panic' in transcript or b'=> ' in transcript:
                    raise RuntimeError('temporary boot failed')
                if b'yckt login:' in transcript:
                    print('Temporary boot reached login; check binding over SSH', flush=True)
                    return
            raise RuntimeError('login prompt not observed')
        except Exception:
            if b'=>' in transcript:
                port.write(b'reset\n')
                print('Requested reset to unchanged default boot', flush=True)
            raise
        finally:
            port.close()


if __name__ == '__main__':
    main()
