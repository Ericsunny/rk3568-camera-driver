#!/usr/bin/env python3
"""Exercise the actual recorder ring with variable packet sizes and ownership."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "evrec.c").read_text(encoding="utf-8")
start = source.index("static int ring_push(")
end = source.index("\nstatic struct rec *ring_at(", start)
function = source[start:end]

prelude = r'''
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_FRAMES 3
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 2
struct rec { unsigned char *data; int len; unsigned seq, pkt; int key;
             double ts_ms, abs_ms, cap_ms; };
static struct rec ring[MAX_FRAMES];
static int ring_head, ring_cnt;
static size_t ring_bytes, ring_limit = 10;
static unsigned long g_pushed;
static FILE *g_ev_fp;
static double now_ms(int c) { return (double)c; }
static void evw_write(const unsigned char *p, int n, unsigned s, unsigned k, double t, double c, int start)
{ (void)p; (void)n; (void)s; (void)k; (void)t; (void)c; (void)start; }
'''

test = r'''
static void check(int condition, const char *message)
{ if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); } }
static unsigned char *packet(size_t n)
{ unsigned char *p = malloc(n); check(p != NULL, "allocation"); memset(p, 1, n); return p; }
int main(void)
{
    unsigned char *p;
    check(!ring_push(packet(4),4,0,0,0,0), "insert first");
    check(!ring_push(packet(4),4,1,0,0,0), "insert second");
    check(!ring_push(packet(2),2,2,0,1,0), "insert third");
    check(!ring_push(packet(7),7,3,0,0,0), "insert after multiple evictions");
    check(ring_cnt == 2 && ring_bytes == 9 && ring[ring_head].len == 2,
          "evict as many old packets as necessary");
    p = packet(11);
    check(ring_push(p,11,4,0,0,0) != 0, "reject packet larger than budget");
    check(ring_cnt == 2 && ring_bytes == 9, "rejection leaves ring intact");
    free(p); /* caller keeps ownership after rejection */
    while (ring_cnt) { free(ring[ring_head].data); ring_head=(ring_head+1)%MAX_FRAMES; ring_cnt--; }
    ring_bytes=0; ring_limit=0;
    p=packet(1);
    check(ring_push(p,1,5,0,0,0) != 0 && !ring_cnt && !ring_bytes,
          "empty zero-budget ring is not evicted");
    free(p);
    puts("ring limits, multiple evictions and ownership: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="evrec-ring-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prelude + function + test, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
