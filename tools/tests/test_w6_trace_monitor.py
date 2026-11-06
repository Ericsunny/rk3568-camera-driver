#!/usr/bin/env python3
"""Host-only parser tests; no board, GPU, NPU or camera."""
import importlib.util
import os
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('w6', Path(__file__).resolve().parents[1] / 'w6_trace_monitor.py')
w6 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w6)
prepare_spec = importlib.util.spec_from_file_location('w6_prepare', Path(__file__).resolve().parents[1] / 'w6_kmsg_prepare.py')
prepare_module = importlib.util.module_from_spec(prepare_spec)
prepare_spec.loader.exec_module(prepare_module)


class TraceTest(unittest.TestCase):
    def test_source_generator_handles_c_format_placeholders(self):
        source = 'int main() {\n' + '\n'.join(
            'fprintf(stderr, "W6_BEGIN:%d\\n", f);\n'
            'fprintf(stderr, "W6_END:%d:%d\\n", f, (int)' + status + ');'
            for status in ('w6_status', 'w6_status', 'st')) + '\n}\n'
        generated = prepare_module.prepare(source)
        self.assertEqual(generated.count('w6_init_marker("BEGIN")'), 3)
        self.assertEqual(generated.count('w6_init_marker("END")'), 3)
        self.assertIn('W6_END:%d:%d\\n', generated)
        with self.assertRaisesRegex(ValueError, 'already instrumented'):
            prepare_module.prepare(generated)

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

    def test_compiled_marker_helper_propagates_open_write_close_failures(self):
        source = r'''
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static int mode, closes;
static int fake_open(const char *path, int flags) {
    if (strcmp(path, "/dev/kmsg") || !(flags & O_WRONLY)) abort();
    return mode == 1 ? -1 : 7;
}
static ssize_t fake_write(int fd, const void *data, size_t len) {
    if (fd != 7 || strncmp((const char *)data, "<6>W6_INIT:", 11)) abort();
    return mode == 2 ? (ssize_t)len - 1 : (ssize_t)len;
}
static int fake_close(int fd) { if (fd != 7) abort(); ++closes; return mode == 3 ? -1 : 0; }
#define open fake_open
#define write fake_write
#define close fake_close
''' + prepare_module.HELPER + r'''
int main() {
    setenv("W6_TOKEN", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    for (mode = 0; mode < 4; ++mode) {
        closes = 0;
        int rc = w6_init_marker("BEGIN");
        if ((mode == 0 ? rc != 0 : rc == 0) || closes != (mode == 1 ? 0 : 1)) return 1;
    }
    unsetenv("W6_TOKEN");
    if (w6_init_marker("END") == 0) return 2;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='w6-helper-') as tmp:
            path = Path(tmp) / 'helper.cpp'; path.write_text(source)
            binary = Path(tmp) / 'helper'
            subprocess.run(['g++', '-std=c++11', '-Wall', '-Werror', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_kernel_markers_do_not_compare_clock_origins(self):
        token = 'a' * 32
        watch = w6.KernelWatch(token)
        for line in (
                'kern :err : [999.0] old unrelated failure',
                'user :info : [1000.0] W6_TRIAL:%s:START' % token,
                'user :info : [1001.0] W6_INIT:%s:123:BEGIN' % token,
                'kern :err : [1001.1] rga2: unknown ioctl cmd!',
                'user :info : [1001.2] W6_INIT:%s:123:END' % token):
            self.assertIsNone(watch.line(line))
        check = w6.TraceCheck()
        for line in (
                '123 9999999999.0 openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>',
                '123 9999999999.1 write(2, "W6_BEGIN:0\\n", 11) = 11',
                '123 9999999999.2 ioctl(0x7, 0x801c7201, 0x123) = -1 EINVAL',
                '123 9999999999.3 ioctl(0x7, 0x601b, 0x123) = 0',
                '123 9999999999.4 ioctl(0x7, 0x5017, 0x123) = 0',
                '123 9999999999.5 write(2, "W6_END:0:1\\n", 11) = 11'):
            check.line(line)
        watch.validate(check, final=True)
        self.assertIn('repeated', watch.line('kern :err : [1002.0] rga2: unknown ioctl cmd!'))
        check.first_pid = 124
        with self.assertRaisesRegex(ValueError, 'PID differs'):
            watch.validate(check, final=True)

    def test_kernel_rejects_missing_late_repeated_and_malformed_markers(self):
        token = 'b' * 32
        start = 'user :info : [1.0] W6_TRIAL:%s:START' % token
        begin = 'user :info : [2.0] W6_INIT:%s:123:BEGIN' % token
        end = 'user :info : [3.0] W6_INIT:%s:123:END' % token
        for prefix in ((start,), (start, begin, end)):
            watch = w6.KernelWatch(token)
            for line in prefix:
                self.assertIsNone(watch.line(line))
            self.assertIn('outside initialization', watch.line('kern :err : [4.0] rga2: unknown ioctl cmd!'))
        watch = w6.KernelWatch(token)
        self.assertIsNone(watch.line(start))
        with self.assertRaisesRegex(ValueError, 'missing'):
            watch.validate(w6.TraceCheck(), final=True)
        self.assertIsNone(watch.line(begin))
        self.assertIn('repeated', watch.line(begin))
        self.assertIn('malformed', watch.line('user :info : [5.0] W6_INIT:%s:garbage' % token))

    def simulate(self, scenario):
        # All kernel writes and hardware processes below are host-only substitutes.
        with tempfile.TemporaryDirectory(prefix='w6-kernel-') as tmp:
            root = Path(tmp); bindir = root / 'bin'; bindir.mkdir()
            kernel = root / 'kernel-stream'; kernel.write_text('')
            logger = bindir / 'dmesg'
            logger.write_text('''#!/usr/bin/env python3
import os, sys, time
with open(os.environ['W6_TEST_KERNEL']) as f:
 while True:
  data = f.read()
  if data: sys.stdout.write(data); sys.stdout.flush()
  if '-w' not in sys.argv: break
  time.sleep(.005)
''')
            logger.chmod(0o755)
            tracer = bindir / 'strace'
            tracer.write_text(r'''#!/usr/bin/env python3
import os, sys, time
scenario=os.environ['W6_TEST_SCENARIO']; token=os.environ['W6_TOKEN']; pid=os.getpid()
def kmsg(message):
 with open(os.environ['W6_TEST_KERNEL'],'a') as f:
  f.write('kern :info : [50000.0] '+message+'\n')
def error():
 with open(os.environ['W6_TEST_KERNEL'],'a') as f:
  f.write('kern :err : [50000.1] rga2: unknown ioctl cmd!\n')
with open(sys.argv[sys.argv.index('-o')+1], 'w', buffering=1) as f:
 def emit(message): f.write('%d 9999999999.0 %s\n' % (pid,message))
 emit('openat(AT_FDCWD, "/dev/rga", O_RDWR) = 7</dev/rga>')
 if scenario == 'stall': time.sleep(10)
 marker_pid=pid+1 if scenario == 'wrong_pid' else pid
 if scenario != 'missing_marker': kmsg('W6_INIT:%s:%d:BEGIN' % (token,marker_pid))
 if scenario == 'kernel_error':
  with open(os.environ['W6_TEST_KERNEL'],'a') as k: k.write('kern :err : [50000.1] new I2C error\n')
 if scenario not in ('missing_marker','late'):
  error()
  if scenario == 'repeat': error()
 for i in range(30):
  emit('write(2, "W6_BEGIN:%d\\n", 11) = 11' % i)
  if i == 0:
   emit('ioctl(0x7, 0x801c7201, 0x123) = -1 EINVAL')
   if scenario != 'missing_probe': emit('ioctl(0x7, 0x601b, 0x123) = 0')
  if scenario != 'missing_ioctl': emit('ioctl(0x7, 0x5017, 0x123) = 0')
  emit('write(2, "W6_END:%d:1\\n", 11) = 11' % i)
  if i == 0 and scenario != 'missing_marker':
   kmsg('W6_INIT:%s:%d:END' % (token,marker_pid))
   if scenario == 'late': error()
  time.sleep(.003)
 if scenario == 'final_error':
  with open(os.environ['W6_TEST_KERNEL'],'a') as k: k.write('kern :err : [50000.2] late I2C error\n')
''')
            tracer.chmod(0o755)
            temperature = root / 'temperature'; temperature.write_text('35000')
            def marker(message):
                if scenario == 'marker_write_failure':
                    raise OSError('injected kernel marker write failure')
                with kernel.open('a') as f:
                    f.write('user :info : [50000.0] '+message+'\n')
            env = {'PATH': str(bindir)+os.pathsep+os.environ['PATH'],
                   'W6_TEST_KERNEL': str(kernel), 'W6_TEST_SCENARIO': scenario}
            boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
            states = [None, '123'] if scenario == 'd_state' else None
            with patch.dict(os.environ, env), patch.object(w6, 'write_kmsg', side_effect=marker), \
                 patch.object(w6, 'find_d_state', side_effect=states, return_value=None), \
                 patch.object(w6, 'FRAME_STALL_SECONDS', .1 if scenario == 'stall' else 8.0):
                rc = w6.run('a', root, root/'unused-binary', boot, os.getpid(), 5,
                            temperature, 70000, 1, 1)
            return rc, (root/'a/monitor.txt').read_text()

    def test_supervisor_success_with_unrelated_clock_origins(self):
        rc, result = self.simulate('success')
        self.assertEqual(rc, 0, result)
        self.assertIn('observed_rga_frames=30', result)
        self.assertIn('rga_init_errors=1', result)

    def test_supervisor_stops_without_weakening_safety_rules(self):
        cases = {'missing_ioctl': 'lacks successful hardware ioctl',
                 'kernel_error': 'new I2C error', 'repeat': 'repeated RGA',
                 'late': 'outside initialization', 'missing_marker': 'missing W6',
                 'missing_probe': 'lacks traced version', 'wrong_pid': 'PID differs',
                 'marker_write_failure': 'marker write failure', 'final_error': 'late I2C error',
                 'd_state': 'D-state process detected', 'stall': 'frame progress stalled'}
        for scenario, expected in cases.items():
            with self.subTest(scenario=scenario):
                rc, result = self.simulate(scenario)
                self.assertEqual(rc, 2, result)
                self.assertIn(expected, result)


if __name__ == '__main__': unittest.main(verbosity=2)
