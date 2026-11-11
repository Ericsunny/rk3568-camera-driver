#!/usr/bin/env python3
"""Compile the actual event pre-roll writer; no vendor headers or camera."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'evrec.c').read_text(encoding='utf-8')
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
struct rec { unsigned char *data; int len; double ts_ms; int key; unsigned seq, pkt; double cap_ms; } records[3];
static FILE *g_ev_fp, *g_ev_packets;
static char g_out_base[512], g_ev_path[512];
static unsigned long g_ev_bytes, g_ev_last_pkts, g_pushed = 3;
static int g_ev_werr, ring_cnt = 3;
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
    if (g_ev_first_ts != 1000 || g_ev_last_ts != 3000 || g_ev_bytes != 6) return 4;
    if (fclose(g_ev_fp)) return 5;
    if (fclose(g_ev_packets)) return 13;
    g_ev_fp=NULL;
    if (evw_open_at(1,2,0,2)) return 6;
    if (g_ev_first_ts != 2000 || g_ev_last_ts != 3000 || g_ev_bytes != 4) return 7;
    if (fclose(g_ev_fp)) return 8;
    if (fclose(g_ev_packets)) return 14;
    puts("pre-roll timestamps and byte count: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='preroll-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(prelude+select_function+function+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test'),str(path)],check=True)
