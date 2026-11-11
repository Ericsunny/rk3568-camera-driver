#!/usr/bin/env python3
"""Check the sensor's actual interval conversion using control stubs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'driver/ov8858_sm/ov8858_sm.c').read_text(encoding='utf-8')
start = source.index('static int ov8858_sm_g_frame_interval(', source.index('static int ov8858_sm_enum_frame_interval'))
end = source.index('\n}', source.index('static int ov8858_sm_s_frame_interval(', start)) + 2
functions = source[start:end]
prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
#define OV8858_SM_PIXEL_RATE 144000000LL
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define clamp_t(t,v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
struct v4l2_subdev { int unused; };
struct v4l2_fract { u32 numerator, denominator; };
struct v4l2_subdev_frame_interval { struct v4l2_fract interval; };
struct v4l2_ctrl { int val; };
struct ov8858_sm_timing { u32 hts,vts,width,height; };
static struct ov8858_sm_timing ov8858_sm_timings[]={{1928,1244,1632,1224},{1940,1236,1920,1080}};
struct ov8858_sm { struct v4l2_subdev subdev; int mutex, is_1080p; struct v4l2_ctrl *vblank; };
static void mutex_lock(int *p) { (void)p; } static void mutex_unlock(int *p) { (void)p; }
static u64 div64_u64(u64 a,u64 b) { return a/b; }
static int __v4l2_ctrl_s_ctrl(struct v4l2_ctrl *c, s32 n) { c->val=n; return 0; }
'''
test = r'''
int main(void) {
    struct v4l2_ctrl v={.val=20};
    struct ov8858_sm s={.vblank=&v};
    struct v4l2_subdev_frame_interval f={{1,15}};
    if (ov8858_sm_s_frame_interval(&s.subdev,&f) || v.val != 1266) return 1;
    if (f.interval.numerator != 2*1928*2490 || f.interval.denominator != 144000000) return 2;
    f.interval.numerator=1; f.interval.denominator=60;
    if (ov8858_sm_s_frame_interval(&s.subdev,&f) || v.val != 20) return 3;
    f.interval.numerator=0;
    if (ov8858_sm_s_frame_interval(&s.subdev,&f) != -EINVAL) return 4;
    if (ov8858_sm_g_frame_interval(&s.subdev,&f) || f.interval.numerator != 2*1928*1244) return 5;
    puts("OV8858 frame interval and VBLANK conversion: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ov8858-interval-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(prelude + functions + test, encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
