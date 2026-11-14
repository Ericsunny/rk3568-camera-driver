#!/usr/bin/env python3
"""Run isolated host fault-injection tests against blocks extracted from evrec.c.

Requires a POSIX host, Python 3 and a C compiler. No camera or vendor libraries
are used. This exercises actual source blocks, not the complete evrec process.
"""
from pathlib import Path
import subprocess
import tempfile

SOURCE = (Path(__file__).resolve().parents[1] / "evrec.c").read_text(encoding="utf-8")


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 0
    for pos in range(opening, len(SOURCE)):
        if SOURCE[pos] == "{":
            depth += 1
        elif SOURCE[pos] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:pos + 1]
    raise ValueError("unterminated function: " + signature)


def block(start, end):
    a = SOURCE.index(start)
    return SOURCE[a:SOURCE.index(end, a)]


EVENT_PRELUDE = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

static FILE *g_ev_fp, *g_ev_packets, *json_fp;
static char g_ev_path[512];
static unsigned char ev_buf[4 << 20];
static unsigned ev_bn, g_ev_event_id, g_ev_segment, g_ev_coalesced, g_ev_cdskip;
static unsigned long g_ev_bytes;
static double g_ev_first_ts, g_ev_last_ts, g_last_detect_ms = 1;
static int g_ev_werr, fault, g_max_events = 1, g_events;
static int g_pre_seconds, g_post_seconds, g_ts_flags;
#define CLOCK_MONOTONIC 1
static double now_ms(int clock_id) { (void)clock_id; return 2000.0; }
static void evw_event_end(const char *reason) { (void)reason; g_events++; }
static long disk_free_mb(void) { return 1234; }

/* Faults affect only the event stream/metadata, not diagnostics to stderr. */
static FILE *inject_fopen(const char *path, const char *mode)
{
    if (fault == 1) { errno = EACCES; return NULL; }
    json_fp = fopen(path, mode);
    return json_fp;
}
static int inject_fprintf(FILE *stream, const char *fmt, ...)
{
    int result;
    va_list ap;
    if (stream == json_fp && fault == 2) {
        /* Simulate a short metadata write reported by fprintf. */
        (void)fwrite("{", 1, 1, stream);
        errno = ENOSPC;
        return -1;
    }
    va_start(ap, fmt);
    result = vfprintf(stream, fmt, ap);
    va_end(ap);
    return result;
}
static int inject_fclose(FILE *stream)
{
    int is_json = stream == json_fp;
    int is_packets = stream == g_ev_packets;
    int result = fclose(stream);
    if (is_json) json_fp = NULL;
    if (is_json && fault == 3) { errno = EIO; return EOF; }
    if (is_packets && fault == 7) { errno = EIO; return EOF; }
    return result;
}
static size_t inject_fwrite(const void *ptr, size_t size, size_t n, FILE *stream)
{
    if (stream == g_ev_fp && fault == 4) {
        errno = ENOSPC;
        return n ? n - 1 : 0;
    }
    return fwrite(ptr, size, n, stream);
}
static int inject_rename(const char *oldpath, const char *newpath)
{
    if (fault == 5 || (fault == 6 && strstr(newpath, ".json"))) {
        errno = EIO; return -1;
    }
    return rename(oldpath, newpath);
}
#define fopen inject_fopen
#define fprintf inject_fprintf
#define fclose inject_fclose
#define fwrite inject_fwrite
#define rename inject_rename
'''

EVENT_TEST = r'''
static int signal_close(int initial_rc, unsigned trig_seq, double trig_ts)
{
    int got_term = 1, rc = initial_rc, reached_max = 0;
    const char *end_reason = "runtime_error";
SIGNAL_BLOCK
EXIT_BLOCK
    return -99;
}
static int normal_close(unsigned trig_seq, double trig_ts)
{
    int rc = 0, reached_max = 0, post_sec = 1;
    for (;;) {
NORMAL_CLOSE_BLOCK
        break;
    }
    (void)reached_max;
    return rc;
}
static void prepare(const char *dir, int which)
{
    char path[600];
    snprintf(g_ev_path, sizeof(g_ev_path), "%s/event%d", dir, which);
    snprintf(path, sizeof(path), "%s.part", g_ev_path);
    g_ev_fp = fopen(path, "wb");
    if (!g_ev_fp) { perror("create .part"); exit(90); }
    g_ev_packets = NULL;
    if (which == 7) {
        snprintf(path, sizeof(path), "%s.packets.part", g_ev_path);
        g_ev_packets = fopen(path, "wb");
        if (!g_ev_packets) exit(91);
    }
    g_ev_event_id = 7; g_ev_segment = 2; g_ev_werr = 0;
    g_ev_bytes = 0; g_ev_first_ts = 1; g_ev_last_ts = 2;
    ev_bn = 1; ev_buf[0] = 42; fault = which; json_fp = NULL;
}
static void check(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main(int argc, char **argv)
{
    int i;
    char path[600];
    struct stat st;
    if (argc != 2) return 2;
    for (i = 0; i <= 7; i++) {
        int rc;
        fault = 0; prepare(argv[1], i);
        rc = signal_close(0, 12, 3.0);
        check(rc == (i ? 1 : 0), "SIGTERM exit status");
        check(g_ev_fp == NULL, "event stream closed");
        check(g_ev_event_id == 7 && g_ev_segment == 2, "event identity retained");
        check(g_ev_werr == (i == 4 || i == 7), "video/sidecar write-error state");
        snprintf(path, sizeof(path), "%s.h264", g_ev_path);
        check((stat(path, &st) == 0) == (i != 4 && i != 5 && i != 7), "video rename status");
        snprintf(path, sizeof(path), "%s.json", g_ev_path);
        check((stat(path, &st) == 0) == (i == 0), "only clean metadata is published");
        if (i == 4 || i == 5 || i == 7) {
            snprintf(path, sizeof(path), "%s.part", g_ev_path);
            check(stat(path, &st) == 0, "failed rename retains part");
        }
        printf("SIGTERM fault=%d rc=%d event_id=%u segment=%u write_error=%d\n",
               i, rc, g_ev_event_id, g_ev_segment, g_ev_werr);
    }
    /* A normal completion must not mark the event complete on metadata error. */
    for (i = 0; i <= 3; i++) {
        int rc;
        fault = 0; prepare(argv[1], i + 10); fault = i;
        g_events = 0;
        rc = normal_close(12, 3.0);
        check(rc == (i ? 1 : 0) && g_events == (i ? 0 : 1) &&
              g_ev_fp == NULL, "normal close status and event count");
        printf("target_left fault=%d rc=%d completed=%d\n", i, rc, g_events);
    }
    return 0;
}
'''

RGA_PRELUDE = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int IM_STATUS;
typedef struct { unsigned char *ptr; } rga_buffer_t;
#define IM_STATUS_SUCCESS 0
#define RK_FORMAT_YCbCr_420_SP 1
#define RK_FORMAT_RGB_888 2
static int g_rga_w = 4, g_rga_h = 4, fail_resize, npu_calls;
static unsigned long g_rga_sum, g_rga_n;
static unsigned char data[64], rgb[64];
static unsigned char *g_rga_buf = data, *g_rgb = rgb;
static rga_buffer_t g_rga_src, g_rga_dst;
static int g_detect = 1, w = 4, h = 4, input_fd = -1;
static int rc, g_streak, g_det_announced, g_trigger;
static const char *end_reason;
static int g_npu_w = 4, g_npu_h = 4;
static struct { unsigned char *map[1]; unsigned stride; } cap;
static struct { int index; } b;
static double t_stage, t_rga, t_npu;
#define CLOCK_MONOTONIC 1
static double now_ms(int clock_id) { (void)clock_id; return 1.0; }
static rga_buffer_t wrapbuffer_virtualaddr_t(void *p, int a, int b, int c, int d, int e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return (rga_buffer_t){p}; }
static rga_buffer_t wrapbuffer_fd_t(int fd, int a, int b, int c, int d, int e)
{ (void)fd; return wrapbuffer_virtualaddr_t(NULL, a, b, c, d, e); }
static IM_STATUS imresize_t(rga_buffer_t a, rga_buffer_t b, int c, int d, int e, int f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return fail_resize; }
static const char *imStrError(int s) { (void)s; return "injected"; }
static int npu_run(const unsigned char *p) { (void)p; npu_calls++; return 0; }
'''

RGA_TEST = r'''
#ifdef HAVE_RKNN
static void run_detection(void)
{
    for (;;) {
RGA_CALLER_BLOCK
        break;
    }
}
#endif
int main(void)
{
    data[0] = 9; rgb[0] = 13;
    cap.map[0] = data;
    cap.stride = 4;
    fail_resize = 1;
#ifdef HAVE_RKNN
    g_streak = 2; g_det_announced = 1; g_trigger = 1;
    run_detection();
    if (npu_calls || g_rga_n || g_streak || g_det_announced || g_trigger ||
        rc != 1 || !end_reason) return 1;
#endif
    rc = rga_scale(data, 4, 4, 4, -1);
    if (rc == 0 || npu_calls || g_rga_n) return 1;
    fail_resize = 0;
#ifdef HAVE_RKNN
    run_detection();
    rc = 0;
    if (npu_calls != 1 || g_rga_sum != 13) return 2;
#else
    rc = rga_scale(data, 4, 4, 4, -1);
    if (g_rga_sum != 9) return 3;
#endif
#ifdef HAVE_RKNN
    if (rc || g_rga_n != 3) return 4;
#else
    if (rc || g_rga_n != 1) return 4;
#endif
    puts("RGA failure skips inference; successful buffer sampled");
    return 0;
}
'''

QBUF_PRELUDE = r'''
#include <stdio.h>
#include <errno.h>
#define VIDIOC_QBUF 1
struct { int fd; } cap;
static int g_stop, g_input_dmabuf, fail_qbuf, recovered, encoded;
static int xioctl(int fd, int req, void *b)
{ (void)fd; (void)req; (void)b; errno = EIO; return fail_qbuf ? -1 : 0; }
static void run_frame(void)
{
    int b;
    for (;;) {
QBUF_BLOCK
        encoded++;
        break;
link_fault:
        recovered++;
        break;
    }
}
int main(void)
{
    fail_qbuf = 1; run_frame();
    if (recovered != 1 || encoded) return 1;
    g_stop = 1; run_frame();
    if (recovered != 1 || encoded) return 2;
    g_stop = 0; fail_qbuf = 0; run_frame();
    if (recovered != 1 || encoded != 1) return 3;
    puts("QBUF failure enters recovery; SIGTERM stops; success encodes");
    return 0;
}
'''


def main():
    signal = block('if (g_ev_fp != NULL) {\n\t\tconst char *reason',
                   '\n\t/* 信号 handler 只置 g_stop')
    exit_block = block('if (got_term && !rc) {', '\n\t/* 触发到 dump')
    normal_close = block('if (g_ev_fp != NULL && g_last_detect_ms > 0 &&',
                         '\n\t\tcontinue;\n\nlink_fault:')
    event = (EVENT_PRELUDE + function('static void ev_flush(void)') + '\n' +
             function('static int evw_close(') + '\n' +
             EVENT_TEST.replace('SIGNAL_BLOCK', signal)
                       .replace('EXIT_BLOCK', exit_block)
                       .replace('NORMAL_CLOSE_BLOCK', normal_close))
    caller = block('if (g_detect) {\n\t\t\tint rga_rc = rga_scale(', '\n#else')
    rga = (RGA_PRELUDE + function('static void letterbox_size(') + '\n' +
           function('static void pad_rgb(') + '\n' + function('static int rga_scale(') + '\n' +
           RGA_TEST.replace('RGA_CALLER_BLOCK', caller))
    qbuf = block('if (!g_input_dmabuf && xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {\n\t\t\tif (g_stop)',
                 '\n\n\t\tif (mpp_frame_init')
    qbuf = QBUF_PRELUDE.replace('QBUF_BLOCK', qbuf)
    with tempfile.TemporaryDirectory(prefix='evrec-host-') as tmp:
        tmp = Path(tmp)
        for name, source, flags in [
            ('event', event, []), ('qbuf', qbuf, []),
            ('rga-only', rga, []), ('rga-rknn', rga, ['-DHAVE_RKNN']),
        ]:
            src = tmp / (name + '.c')
            binary = tmp / name
            src.write_text(source, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', *flags,
                            str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary), *([str(tmp)] if name == 'event' else [])],
                           check=True)
            print(name + ': PASS', flush=True)


if __name__ == '__main__':
    main()
