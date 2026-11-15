#!/usr/bin/env python3
"""Exercise extracted production functions with host substitutes, without a camera."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise ValueError('unterminated function: ' + signature)


PRELUDE = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PKB_MAX 64
struct pktbuf { unsigned char *data; int len; unsigned seq; unsigned char key;
                uint32_t ts90; uint64_t tag; };
static struct pktbuf g_pkb[PKB_MAX];
static int g_pkb_started=1, g_pkb_depth=30, g_pkb_mb=8, g_rtsp_port;
static uint64_t g_pkb_oldest_tag, g_pkb_next_tag;
static unsigned long g_pkb_bytes, g_pkb_in, g_pkb_drop;
static int g_pkb_lock, g_nal_lock, g_pkb_cond, g_sps_len, g_pps_len, g_ts_off;
static void pthread_mutex_lock(int *p) { (void)p; }
static void pthread_mutex_unlock(int *p) { (void)p; }
static void pthread_cond_broadcast(int *p) { (void)p; }
static void rtsp_scan_nals(const unsigned char *p, int n) { (void)p; (void)n; }

struct capture { unsigned w, h, stride; int nbufs; size_t len[4]; };
static struct capture cap, next_cap;
static size_t g_fsize;
static int g_stop, g_recover=2, released, drained, notified, idr;
#define MPP_ENC_SET_IDR_FRAME 1
struct api { void (*control)(void *, int, void *); };
static void control(void *ctx, int cmd, void *p) { (void)ctx; (void)cmd; (void)p; idr++; }
static struct api api = { control }, *g_mpi = &api;
static void *g_ctx;
static void cap_release(void) { released++; }
static int cap_open(const char *dev, int w, int h, int n) {
    (void)dev; (void)w; (void)h; (void)n; cap=next_cap; return 0;
}
static void enc_drain_packets(void) { drained++; }
static void rtsp_hub_notify_recover(void) { notified++; }
'''

TEST = r'''
static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void clear_ring(void) {
    while (g_pkb_oldest_tag < g_pkb_next_tag)
        free(g_pkb[g_pkb_oldest_tag++ % PKB_MAX].data);
    memset(g_pkb, 0, sizeof(g_pkb));
    g_pkb_oldest_tag=g_pkb_next_tag=g_pkb_bytes=g_pkb_in=g_pkb_drop=0;
}
static void recovery_case(unsigned w, unsigned h, unsigned stride,
                          size_t source_len, size_t capacity, int success) {
    cap.w=1920; cap.h=1080; cap.stride=1920;
    next_cap.w=w; next_cap.h=h; next_cap.stride=stride;
    next_cap.nbufs=1; next_cap.len[0]=source_len;
    g_fsize=capacity; released=drained=notified=idr=0;
    check((link_recover("fake",1920,1080,0) > 0) == success, "recover result");
    check(drained == success && notified == success && idr == success,
          "do not restart consumers on incompatible format");
    check(released == (success ? 1 : 2), "capture cleanup");
}
int main(void) {
    size_t mb=1024u*1024u;
    unsigned char *data=malloc(9*mb);
    check(data != NULL, "allocate input"); memset(data, 0x55, 9*mb);
    pkb_push(data,3*(int)mb,1,0,0); pkb_push(data,3*(int)mb,2,0,0);
    pkb_push(data,3*(int)mb,3,0,0);
    check(g_pkb_oldest_tag==1 && g_pkb_next_tag==3 && g_pkb_bytes==6*mb &&
          g_pkb[1].seq==2 && g_pkb[2].seq==3 && g_pkb_drop==1,
          "byte pressure evicts only the oldest packet");
    clear_ring();
    pkb_push(data,3*(int)mb,1,0,0); pkb_push(data,3*(int)mb,2,0,0);
    pkb_push(data,2*(int)mb,3,0,0);
    check(g_pkb_oldest_tag==0 && g_pkb_bytes==8*mb,
          "exact byte limit does not evict");
    pkb_push(data,3*(int)mb,4,0,0);
    check(g_pkb_oldest_tag==1 && g_pkb_bytes==8*mb && g_pkb_drop==1,
          "one eviction reaches exact byte limit");
    clear_ring();
    pkb_push(data,3*(int)mb,1,0,0); pkb_push(data,3*(int)mb,2,0,0);
    pkb_push(data,1*(int)mb,3,0,0); pkb_push(data,5*(int)mb,4,0,0);
    check(g_pkb_oldest_tag==2 && g_pkb_bytes==6*mb && g_pkb_drop==2,
          "multiple evictions retain the remaining packet");
    clear_ring();
    g_pkb_depth=2;
    pkb_push(data,1,1,0,0); pkb_push(data,1,2,0,0); pkb_push(data,1,3,0,0);
    check(g_pkb_oldest_tag==1 && g_pkb_bytes==2 && g_pkb_drop==1,
          "packet-count pressure still evicts oldest");
    clear_ring(); g_pkb_depth=30;
    pkb_push(data,1,1,0,0); pkb_push(data,9*(int)mb,2,0,0);
    check(g_pkb_oldest_tag==1 && g_pkb_next_tag==2 && g_pkb_bytes==9*mb,
          "existing oversized-packet policy remains unchanged");
    clear_ring(); free(data);
    recovery_case(1920,1080,1920,3110400,3110400,1);
    recovery_case(1920,1080,2048,3317760,3110400,0);
    recovery_case(1280,1080,1920,3110400,3110400,0);
    recovery_case(1920,720,1920,3110400,3110400,0);
    recovery_case(1920,1080,1920,3110399,3110400,0);
    recovery_case(1920,1080,1920,3110400,3110399,0);
    puts("RTSP eviction and recovery format contracts: PASS");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', choices=('rtsp', 'recovery'),
                        help='run against the HEAD version to verify test sensitivity')
    args = parser.parse_args()
    evrec = (ROOT / 'evrec.c').read_text(encoding='utf-8')
    rtsp = (ROOT / 'rtsp_hub.c').read_text(encoding='utf-8')
    if args.baseline == 'rtsp':
        rtsp = subprocess.check_output(['git', 'show', 'HEAD:tools/rtsp_hub.c'],
                                       cwd=ROOT.parent).decode('utf-8')
    elif args.baseline == 'recovery':
        old = subprocess.check_output(['git', 'show', 'HEAD:tools/evrec.c'],
                                      cwd=ROOT.parent).decode('utf-8')
        evrec = evrec.replace(function(evrec, 'static int link_recover('),
                              function(old, 'static int link_recover('), 1)
    code = (PRELUDE + function(rtsp, 'void pkb_push(') + '\n' +
            function(evrec, 'static int capture_matches_encoder(') + '\n' +
            function(evrec, 'static int link_recover(') + '\n' + TEST)
    with tempfile.TemporaryDirectory(prefix='camera-contract-') as tmp:
        path = Path(tmp)
        source = path / 'test.c'
        source.write_text(code, encoding='utf-8')
        if os.name == 'nt' and shutil.which('cl'):
            subprocess.run(['cl', '/nologo', '/std:c11', '/W4', '/utf-8',
                            str(source), '/Fe:' + str(path / 'test.exe')],
                           cwd=path, check=True)
            executable = path / 'test.exe'
        else:
            executable = path / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(source), '-o', str(executable)],
                           cwd=path, check=True)
        subprocess.run([str(executable)], cwd=path, check=True)


if __name__ == '__main__':
    main()
