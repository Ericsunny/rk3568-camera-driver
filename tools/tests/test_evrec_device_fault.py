#!/usr/bin/env python3
"""Linker-wrapper tests with fake devices; extracts real evrec branches."""
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'tools/evrec.c').read_text(encoding='utf-8')


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index('{', start)
    depth = 0
    for pos in range(opening, len(SOURCE)):
        if SOURCE[pos] == '{':
            depth += 1
        elif SOURCE[pos] == '}':
            depth -= 1
            if not depth:
                return SOURCE[start:pos + 1]
    raise ValueError(signature)


def block(start, end):
    begin = SOURCE.index(start)
    return SOURCE[begin:SOURCE.index(end, begin)]


FAKE_HEADER = r'''
#ifndef TEST_IM2D_H
#define TEST_IM2D_H
typedef enum { IM_STATUS_SUCCESS = 1, IM_STATUS_FAILED = 0 } IM_STATUS;
typedef struct { unsigned char *ptr; } rga_buffer_t;
#endif
'''

PRELUDE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <im2d.h>
extern int __wrap_ioctl(int, unsigned long, ...);
extern IM_STATUS __wrap_imresize_t(const rga_buffer_t, rga_buffer_t, double, double, int, int);
static int g_stop, real_qbuf, real_dqbuf, real_other, real_rga, npu_calls, encoded;
static struct { int fd; unsigned char *map[1]; } cap;
static struct { int index; } b;
static int g_detect = 1, w = 4, h = 4, g_rga_w = 4, g_rga_h = 4;
static unsigned long g_rga_sum, g_rga_n;
static unsigned char data[64], rgb[64], *g_rgb = rgb;
static rga_buffer_t g_rga_src, g_rga_dst;
static double t_stage, t_rga, t_npu;
#define RK_FORMAT_YCbCr_420_SP 1
static double now_ms(int id) { (void)id; return 1.0; }
static void npu_run(const unsigned char *p) { (void)p; npu_calls++; }
static rga_buffer_t wrapbuffer_virtualaddr_t(void *p, unsigned a, unsigned b_,
                                              unsigned c, unsigned d, int e)
{ (void)a; (void)b_; (void)c; (void)d; (void)e; return (rga_buffer_t){p}; }
static const char *imStrError(IM_STATUS s) { (void)s; return "injected"; }
int __real_ioctl(int fd, unsigned long req, ...)
{
    (void)fd;
    if (req == VIDIOC_QBUF) real_qbuf++;
    else if (req == VIDIOC_DQBUF) real_dqbuf++;
    else real_other++;
    return 0;
}
IM_STATUS __real_imresize_t(const rga_buffer_t src, rga_buffer_t dst,
                            double fx, double fy, int interp, int sync)
{
    (void)src; (void)dst; (void)fx; (void)fy; (void)interp; (void)sync;
    real_rga++; return IM_STATUS_SUCCESS;
}
#define ioctl __wrap_ioctl
'''

BODY = r'''
#undef ioctl
#define imresize_t __wrap_imresize_t
RGA_SCALE
#undef imresize_t
static void run_detection(void) {
RGA_CALLER
}
static int run_requeue(void) {
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    for (;;) {
QBUF_BLOCK
        encoded++;
        return 0;
link_fault:
        return 1;
    }
    return 2;
}
int main(int argc, char **argv) {
    struct v4l2_buffer v;
    int other, rc;
    if (argc != 2) return 80;
    memset(&v, 0, sizeof(v));
    cap.fd = open("/dev/null", O_RDWR);
    other = open("/dev/zero", O_RDWR);
    if (cap.fd < 0 || other < 0) return 81;
    cap.map[0] = data; rgb[0] = 13;
    if (!strncmp(argv[1], "qbuf", 4)) {
        /* Initial queue and a different device are not eligible. */
        if (__wrap_ioctl(cap.fd, VIDIOC_QBUF, &v) ||
            __wrap_ioctl(other, VIDIOC_DQBUF, &v) ||
            __wrap_ioctl(other, VIDIOC_QBUF, &v)) return 82;
        if (!strcmp(argv[1], "qbuf2")) {
            if (xioctl(cap.fd, VIDIOC_DQBUF, &v) || run_requeue() != 0 ||
                encoded != 1 || real_qbuf != 3) return 88;
            encoded = 0;
            real_qbuf = 2; /* normalize only the test's forwarding counter */
        }
        if (xioctl(cap.fd, VIDIOC_DQBUF, &v)) return 83;
        rc = run_requeue();
        if (rc != 1 || encoded || real_qbuf != 2) return 84;
        if (xioctl(cap.fd, VIDIOC_DQBUF, &v) || run_requeue() != 0 ||
            encoded != 1 || real_qbuf != 3) return 85;
        printf("QBUF recovery path and one-shot forwarding PASS\n");
    } else {
        if (!strcmp(argv[1], "rga2")) {
            run_detection();
            if (npu_calls != 1 || real_rga != 1 || g_rga_n != 1) return 89;
            npu_calls = real_rga = 0; g_rga_n = g_rga_sum = 0;
        }
        run_detection();
        if (npu_calls || real_rga || g_rga_n) return 86;
        run_detection();
        if (npu_calls != 1 || real_rga != 1 || g_rga_n != 1 || g_rga_sum != 13) return 87;
        printf("RGA failure skips NPU; next frame runs PASS\n");
    }
    close(other); close(cap.fd); return 0;
}
'''

LINK_PROBE = r'''
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <im2d.h>
extern IM_STATUS imresize_t(const rga_buffer_t, rga_buffer_t, double, double, int, int);
int main(int argc, char **argv) {
    struct v4l2_buffer b = {0};
    rga_buffer_t image = {0};
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0 || argc != 2) return 1;
    if (argv[1][0] == 'q') {
        if (ioctl(fd, VIDIOC_DQBUF, &b) || ioctl(fd, VIDIOC_QBUF, &b) != -1 || errno != EIO)
            return 2;
        if (ioctl(fd, VIDIOC_DQBUF, &b) || ioctl(fd, VIDIOC_QBUF, &b)) return 3;
    } else {
        if (imresize_t(image, image, 1, 1, 0, 1) != IM_STATUS_FAILED ||
            imresize_t(image, image, 1, 1, 0, 1) != IM_STATUS_SUCCESS) return 4;
    }
    close(fd);
    return 0;
}
'''

LINK_STUB = r'''
#include <sys/ioctl.h>
#include <im2d.h>
int ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return 0; }
IM_STATUS imresize_t(const rga_buffer_t src, rga_buffer_t dst,
                     double fx, double fy, int interpolation, int sync) {
    (void)src; (void)dst; (void)fx; (void)fy; (void)interpolation; (void)sync;
    return IM_STATUS_SUCCESS;
}
'''


def main():
    scale = function('static int rga_scale(')
    caller = block('if (g_detect) {\n\t\t\tint rga_rc = rga_scale(', '\n#else')
    qbuf = block('if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {\n\t\t\tif (g_stop)',
                 '\n\n\t\tmpp_frame_init')
    source = (PRELUDE + function('static int xioctl(') + '\n' +
              BODY.replace('RGA_SCALE', scale).replace('RGA_CALLER', caller)
                  .replace('QBUF_BLOCK', qbuf))
    with tempfile.TemporaryDirectory(prefix='evdf-test-') as tmp:
        tmp = Path(tmp)
        (tmp / 'im2d.h').write_text(FAKE_HEADER, encoding='utf-8')
        (tmp / 'harness.c').write_text(source, encoding='utf-8')
        binary = tmp / 'harness'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DHAVE_RKNN', '-I', str(tmp), '-pthread',
                        str(ROOT / 'tools/evrec_device_fault_wrap.c'),
                        str(tmp / 'harness.c'), '-o', str(binary)], check=True)
        for mode in ('qbuf', 'rga', 'qbuf2', 'rga2'):
            env = os.environ.copy()
            fault = mode.rstrip('2')
            env.update(EVDF_MODE=fault, EVDF_HIT='2' if mode.endswith('2') else '1',
                       EVDF_CAMERA='/dev/null')
            result = subprocess.run([str(binary), mode], env=env, text=True, capture_output=True)
            expected = 'qbuf-after-dqbuf' if fault == 'qbuf' else 'imresize'
            assert result.returncode == 0, (mode, result.stdout, result.stderr, result.returncode)
            assert result.stderr.count('[evdf] HIT ') == 1 and 'operation=' + expected in result.stderr
            assert 'real=not-called' in result.stderr and 'reported=' in result.stderr and \
                ('occurrence=2' if mode.endswith('2') else 'occurrence=1') in result.stderr
            print(mode + ': ' + result.stdout.strip())
        env['EVDF_HIT'] = '0'
        result = subprocess.run([str(binary), 'qbuf'], env=env, text=True, capture_output=True)
        assert result.returncode == 125 and 'CONFIG ERROR' in result.stderr
        print('invalid configuration fails closed: PASS')
        (tmp / 'probe.c').write_text(LINK_PROBE, encoding='utf-8')
        (tmp / 'stub.c').write_text(LINK_STUB, encoding='utf-8')
        linked = tmp / 'linked-probe'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(tmp), '-pthread', str(tmp / 'probe.c'),
                        str(tmp / 'stub.c'), str(ROOT / 'tools/evrec_device_fault_wrap.c'),
                        '-Wl,--wrap=ioctl', '-Wl,--wrap=imresize_t', '-o', str(linked)], check=True)
        for fault in ('qbuf', 'rga'):
            env.update(EVDF_MODE=fault, EVDF_HIT='1', EVDF_CAMERA='/dev/null')
            result = subprocess.run([str(linked), fault], env=env, text=True, capture_output=True)
            assert result.returncode == 0 and result.stderr.count('[evdf] HIT ') == 1, (
                fault, result.returncode, result.stderr)
            print(fault + ': actual GNU ld --wrap redirection PASS')


if __name__ == '__main__':
    main()
