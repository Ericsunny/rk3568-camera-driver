#!/usr/bin/env python3
"""Host-only parser tests; no board, GPU, NPU or camera."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('w6', Path(__file__).resolve().parents[1] / 'w6_trace_monitor.py')
w6 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w6)


class TraceTest(unittest.TestCase):
    def test_successful_ioctl_between_each_marker(self):
        check = w6.TraceCheck()
        check.line('123 openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>')
        for i in range(30):
            check.line('123 write(2<pipe:[7]>, "W6_BEGIN:%d\\n", 11) = 11' % i)
            check.line('123 ioctl(0x7, 0x5017, 0x123) = 0')
            check.line('123 write(2<pipe:[7]>, "W6_END:%d:1\\n", 11) = 11' % i)
        check.finish(30)

    def test_missing_or_failed_hardware_submission_stops(self):
        for ioctl in ('', 'ioctl(0x7, 0x5017, 0x123) = -1 EIO',
                      'ioctl(0x8, 0x5017, 0x123) = 0'):
            check = w6.TraceCheck()
            check.line('openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>')
            check.line('write(2, "W6_BEGIN:0\\n", 11) = 11')
            check.line(ioctl)
            with self.assertRaises(ValueError):
                check.line('write(2, "W6_END:0:1\\n", 11) = 11')

    def test_closed_device_and_failed_rga_return(self):
        check = w6.TraceCheck()
        check.line('openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>')
        check.line('close(7</dev/rga<char 10:58>>) = 0')
        check.line('write(2, "W6_BEGIN:0\\n", 11) = 11')
        check.line('ioctl(0x7, 0x5017, 0x123) = 0')
        with self.assertRaises(ValueError):
            check.line('write(2, "W6_END:0:1\\n", 11) = 11')
        check = w6.TraceCheck()
        check.line('openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>')
        check.line('write(2, "W6_BEGIN:0\\n", 11) = 11')
        check.line('ioctl(0x7, 0x5017, 0x123) = 0')
        with self.assertRaises(ValueError):
            check.line('write(2, "W6_END:0:0\\n", 11) = 11')

    def test_kernel_whitelist_is_narrow_and_time_bounded(self):
        self.assertIsNone(w6.kernel_problem('kern :err : [10.0] old unrelated failure', 11))
        self.assertIsNone(w6.kernel_problem('kern :err : [12.0] imx415 1-001a: write reg array error', 11))
        self.assertIsNotNone(w6.kernel_problem('kern :err : [12.0] other I2C error', 11))
        self.assertIsNotNone(w6.kernel_problem('kern :warn : [12.0] unknown warning', 11))
        self.assertIsNotNone(w6.kernel_problem('kern :err : missing timestamp', 11))
        self.assertIsNotNone(w6.kernel_problem('kern :err : [12.0] imx415 1-001a: write reg array error extra', 11))

    def test_app_whitelist_does_not_hide_cpu_fallback(self):
        self.assertIsNone(w6.app_problem(w6.KNOWN_RGA))
        self.assertIsNotNone(w6.app_problem('RGA fallback to CPU software'))
        self.assertIsNotNone(w6.app_problem(w6.KNOWN_RGA + ' other failure'))

    def test_preflight_rejects_hot_or_full(self):
        with tempfile.TemporaryDirectory(prefix='w6-resource-') as tmp:
            root = Path(tmp); temperature = root / 'temperature'
            temperature.write_text('70000')
            with self.assertRaisesRegex(ValueError, 'temperature'):
                w6.check_resources(root, temperature, 70000, 1, 1)
            temperature.write_text('35000')
            with self.assertRaisesRegex(ValueError, 'storage'):
                w6.check_resources(root, temperature, 70000, 2**62, 1)

    def test_kernel_rga_allowance_is_single_and_within_first_frame(self):
        line = 'kern :err : [100.250000] rga2: unknown ioctl cmd!'
        check = w6.TraceCheck()
        check.first_begin_epoch = 1100.2
        check.first_end_epoch = 1100.3
        check.version_query_epoch = 1100.24
        check.compat_query_epoch = 1100.26
        watch = w6.KernelWatch(100.0, 1000.0)
        self.assertIsNone(watch.line(line))
        watch.validate(check, final=True)
        self.assertIn('repeated', watch.line(line))
        late = w6.KernelWatch(100.0, 1000.0)
        self.assertIsNone(late.line('kern :err : [101.000000] rga2: unknown ioctl cmd!'))
        with self.assertRaisesRegex(ValueError, 'outside first-frame'):
            late.validate(check, final=True)
        missing = w6.KernelWatch(100.0, 1000.0)
        self.assertIsNone(missing.line(line))
        with self.assertRaisesRegex(ValueError, 'cannot be correlated'):
            missing.validate(w6.TraceCheck(), final=True)
        unexpected = w6.KernelWatch(100.0, 1000.0)
        self.assertIsNotNone(unexpected.line('kern :err : [100.250000] rga2: unknown ioctl cmd! extra'))

    def test_supervisor_with_simulated_tracer_and_logger(self):
        with tempfile.TemporaryDirectory(prefix='w6-supervisor-') as tmp:
            root = Path(tmp); bindir = root / 'bin'; bindir.mkdir()
            fake_dmesg = bindir / 'dmesg'
            fake_dmesg.write_text('#!/usr/bin/env python3\nimport time\nwhile True: time.sleep(.1)\n')
            fake_dmesg.chmod(0o755)
            fake_strace = bindir / 'strace'
            fake_strace.write_text('''#!/usr/bin/env python3
import sys, time
path=sys.argv[sys.argv.index('-o')+1]
with open(path,'w',buffering=1) as f:
 f.write('openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>\\n')
 for i in range(30):
  f.write('write(2, "W6_BEGIN:%d\\\\n", 11) = 11\\n' % i)
  f.write('ioctl(0x7, 0x5017, 0x123) = 0\\n')
  f.write('write(2, "W6_END:%d:1\\\\n", 11) = 11\\n' % i)
  time.sleep(.003)
''')
            fake_strace.chmod(0o755)
            temperature = root / 'temperature'; temperature.write_text('35000')
            old_path = os.environ['PATH']; os.environ['PATH'] = str(bindir) + os.pathsep + old_path
            try:
                boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
                # Isolate the host's unrelated processes; production D-state check is unchanged.
                with patch.object(w6, 'find_d_state', return_value=None):
                    rc = w6.run('a', root, root / 'unused-binary', boot, os.getpid(), 5,
                                temperature, 70000, 1, 1)
                self.assertEqual(rc, 0, (root / 'a/monitor.txt').read_text())
                self.assertIn('observed_rga_frames=30', (root / 'a/monitor.txt').read_text())
            finally:
                os.environ['PATH'] = old_path

    def test_supervisor_stops_on_missing_ioctl_and_new_kernel_error(self):
        for scenario in ('missing_ioctl', 'kernel_error', 'd_state', 'stall'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory(prefix='w6-stop-') as tmp:
                root = Path(tmp); bindir = root / 'bin'; bindir.mkdir()
                fake_dmesg = bindir / 'dmesg'
                if scenario == 'kernel_error':
                    fake_dmesg.write_text('#!/usr/bin/env python3\nimport time\n'
                                          'time.sleep(.5)\n'
                                          'print("kern :err : [%.6f] new I2C error" % '
                                          'time.clock_gettime(time.CLOCK_BOOTTIME),flush=True)\n'
                                          'time.sleep(10)\n')
                else:
                    fake_dmesg.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(10)\n')
                fake_dmesg.chmod(0o755)
                fake_strace = bindir / 'strace'
                fake_strace.write_text('''#!/usr/bin/env python3
import sys,time
path=sys.argv[sys.argv.index('-o')+1]
with open(path,'w',buffering=1) as f:
 f.write('openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>\\n')
 f.write('write(2, "W6_BEGIN:0\\\\n", 11) = 11\\n')
 if '%s' == 'missing_ioctl': f.write('write(2, "W6_END:0:1\\\\n", 11) = 11\\n')
 time.sleep(10)
''' % scenario)
                fake_strace.chmod(0o755)
                temperature = root / 'temperature'; temperature.write_text('35000')
                old_path = os.environ['PATH']; os.environ['PATH'] = str(bindir) + os.pathsep + old_path
                try:
                    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
                    states = [None, '123'] if scenario == 'd_state' else None
                    with patch.object(w6, 'find_d_state', side_effect=states, return_value=None), \
                         patch.object(w6, 'FRAME_STALL_SECONDS', .1 if scenario == 'stall' else 8.0):
                        rc = w6.run('a', root, root / 'unused-binary', boot, os.getpid(), 5,
                                    temperature, 70000, 1, 1)
                    result = (root / 'a/monitor.txt').read_text()
                    self.assertEqual(rc, 2, result)
                    expected = {'missing_ioctl': 'lacks successful hardware ioctl',
                                'kernel_error': 'new kernel error',
                                'd_state': 'D-state process detected: 123',
                                'stall': 'frame progress stalled'}
                    self.assertIn(expected[scenario], result)
                finally:
                    os.environ['PATH'] = old_path


if __name__ == '__main__': unittest.main(verbosity=2)
