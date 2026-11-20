#!/usr/bin/env python3
"""Host integration of actual evrec event functions and writer queue."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/evrec.c').read_text(encoding='utf-8')


def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == '{': depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth: return source[start:pos + 1]
    raise ValueError(signature)


prelude = r'''
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "evrec_writer.h"
#define EV_BUF_CAP 8u
#define EVW_QUEUE_MAX_BYTES 1024u
#define EVW_QUEUE_MAX_PACKETS 16u
struct rec { unsigned char *data; int len; double ts_ms; int key; unsigned seq, pkt; double cap_ms; } records[3];
static FILE *g_ev_fp, *g_ev_packets;
static char g_out_base[512], g_ev_path[512];
static unsigned long g_ev_bytes, g_ev_last_pkts, g_pushed = 3;
static atomic_int g_ev_werr;
static int ring_cnt = 3, fail_video, fail_close, g_pre_seconds=2, g_post_seconds=2, g_ts_flags=1;
static unsigned char ev_buf[EV_BUF_CAP];
static unsigned ev_bn, g_ev_event_id, g_ev_segment, g_ev_coalesced, g_ev_cdskip;
static double g_ev_first_ts, g_ev_last_ts;
static int g_detect_pipeline;
static unsigned g_event_side_generation;
static struct { unsigned seq, epoch; } g_event_trigger_source;
static unsigned long long g_event_trigger_applied_ns;
static struct evw_queue g_ev_queue = EVW_QUEUE_INITIALIZER;
static long disk_free_mb(void) { return 1000; }
static int ring_last_idr(void) { return 0; }
static struct rec *ring_at(int i) { return &records[i]; }
static size_t test_fwrite(const void *p, size_t size, size_t count, FILE *f) {
    if (f == g_ev_fp && fail_video) { errno=ENOSPC; return 0; }
    return fwrite(p,size,count,f);
}
static int test_fclose(FILE *f) {
    int video = f == g_ev_fp;
    int rc = fclose(f);
    if (video && fail_close) { errno=EIO; return EOF; }
    return rc;
}
#define fwrite test_fwrite
#define fclose test_fclose
'''

test = r'''
static int exists(const char *p) { struct stat s; return stat(p,&s)==0; }
int main(int argc,char **argv) {
    unsigned char a[]="A",b[]="B",c[]="C",d[]="D";
    char path[600], got[8]={0}; FILE *f;
    if(argc!=2) return 2;
    snprintf(g_out_base,sizeof(g_out_base),"%s/event",argv[1]);
    records[0]=(struct rec){a,1,1000,1,0,0,1000};
    records[1]=(struct rec){b,1,2000,0,1,0,2000};
    records[2]=(struct rec){c,1,3000,0,2,0,3000};
    if(evw_open_at(0,1,0,1)) return 3;
    a[0]='x'; b[0]='x'; c[0]='x';
    g_pushed=4; evw_write(d,1,3,0,4000,4000,0); d[0]='x';
    if(evw_close("target_left",3,4000) || g_ev_werr) return 4;
    snprintf(path,sizeof(path),"%s/event-01.h264",argv[1]);
    f=fopen(path,"rb"); if(!f || fread(got,1,4,f)!=4) return 5;
    fclose(f); if(memcmp(got,"ABCD",4)) return 6;
    snprintf(path,sizeof(path),"%s/event-01.json",argv[1]); if(!exists(path)) return 7;
    a[0]='A'; b[0]='B'; c[0]='C'; fail_video=1;
    if(evw_open_at(0,2,0,2)) return 8;
    if(!evw_close("sigterm",3,4000) || !g_ev_werr) return 9;
    snprintf(path,sizeof(path),"%s/event-02.part",argv[1]); if(!exists(path)) return 10;
    snprintf(path,sizeof(path),"%s/event-02.json",argv[1]); if(exists(path)) return 11;
    if(evw_queue_failed(&g_ev_queue)) return 12; /* stopped queue */
    fail_video=0; fail_close=1;
    if(evw_open_at(0,3,0,3)) return 13;
    if(!evw_close("sigterm",3,4000) || !g_ev_werr) return 14;
    snprintf(path,sizeof(path),"%s/event-03.part",argv[1]); if(!exists(path)) return 15;
    snprintf(path,sizeof(path),"%s/event-03.json",argv[1]); if(exists(path)) return 16;
    puts("async event: ordered pre/post, publication and failed-write propagation PASS");
    return 0;
}
'''

names = ('static void ev_flush(', 'static int evw_consume(',
         'static int evw_finish_io(', 'static int evw_enqueue(',
         'static void evw_write(', 'static int evw_open_at(', 'static int evw_close(')
with tempfile.TemporaryDirectory(prefix='async-event-') as name:
    tmp = Path(name)
    (tmp / 'test.c').write_text(prelude + '\n'.join(function(n) for n in names) + test)
    subprocess.run(['cc', '-std=c11', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'tools'), str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test'), str(tmp)], check=True)
