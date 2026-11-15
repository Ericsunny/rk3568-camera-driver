#!/usr/bin/env python3
"""Compile the actual event pre-roll writer; no vendor headers or camera."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'evrec.c').read_text(encoding='utf-8')
def extract(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == '{': depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth: return source[start:pos + 1]
    raise ValueError(signature)
start = source.index('static int evw_open_at(')
end = source.index('\n/* 新事件:', start)
function = source[start:end]
select_start = source.index('static int ring_preroll_start(')
select_end = source.index('\n}', select_start) + 2
select_function = source[select_start:select_end]
prelude = r'''
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include "evrec_writer.h"
#define EV_BUF_CAP (4u << 20)
#define EVW_QUEUE_MAX_BYTES (64u << 20)
#define EVW_QUEUE_MAX_PACKETS 4096u
struct rec { unsigned char *data; int len; double ts_ms; int key; unsigned seq, pkt; double cap_ms; } records[3];
static FILE *g_ev_fp, *g_ev_packets;
static char g_out_base[512], g_ev_path[512];
static unsigned long g_ev_bytes, g_ev_last_pkts, g_pushed = 3;
static atomic_int g_ev_werr;
static int ring_cnt = 3;
static unsigned char ev_buf[EV_BUF_CAP];
static unsigned ev_bn;
static struct evw_queue g_ev_queue = EVW_QUEUE_INITIALIZER;
static double g_ev_first_ts, g_ev_last_ts;
static unsigned g_ev_event_id, g_ev_segment;
static int ring_last_idr(void) { return 0; }
static struct rec *ring_at(int i) { return &records[i]; }
'''
test = r'''
int main(int argc, char **argv) {
    unsigned char data[] = {1,2};
    int i;
    if (argc != 2) return 2;
    snprintf(g_out_base, sizeof(g_out_base), "%s/event", argv[1]);
    for (i=0; i<3; i++) records[i]=(struct rec){data,2,1000.0+i*1000,0,(unsigned)i,0,1000.0+i*1000};
    if (ring_preroll_start(3000, 2) != -1) return 9;
    records[0].key=1; records[1].key=1; records[2].key=1;
    if (ring_preroll_start(3000, 2) != 0) return 10;
    if (ring_preroll_start(3000, 0) != 2) return 11;
    if (ring_preroll_start(2999, 2) != -1) return 12;
    if (evw_open_at(0,1,0,1)) return 3;
    if (evw_queue_failed(&g_ev_queue)) return 19;
    data[0]=9; data[1]=9;
    if (evw_queue_stop(&g_ev_queue)) return 15;
    if (g_ev_first_ts != 1000 || g_ev_last_ts != 3000 || g_ev_bytes != 6) return 4;
    if (g_ev_fp || g_ev_packets) return 5;
    { char path[600]; unsigned char got[6]; FILE *f;
      snprintf(path,sizeof(path),"%s/event-01.part",argv[1]);
      f=fopen(path,"rb"); if (!f || fread(got,1,6,f)!=6) return 16;
      fclose(f); for (i=0;i<6;i++) if (got[i] != (i%2 ? 2 : 1)) return 17; }
    g_ev_fp=NULL;
    if (evw_open_at(1,2,0,2)) return 6;
    if (evw_queue_stop(&g_ev_queue)) return 18;
    if (g_ev_first_ts != 2000 || g_ev_last_ts != 3000 || g_ev_bytes != 4) return 7;
    if (g_ev_fp || g_ev_packets) return 8;
    puts("pre-roll timestamps and byte count: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='preroll-') as tmp:
    path = Path(tmp)
    writer = '\n'.join(extract(s) for s in ('static void ev_flush(',
        'static int evw_consume(', 'static int evw_finish_io(', 'static int evw_enqueue('))
    (path/'test.c').write_text(prelude+writer+select_function+function+test)
    subprocess.run(['cc','-std=c11','-pthread','-Wall','-Wextra','-Werror',
                    '-I',str(Path(__file__).resolve().parents[1]),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test'),str(path)],check=True)
