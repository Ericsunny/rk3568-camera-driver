#!/usr/bin/env python3
"""Check RKAIQ zero-size and V4L2 size-specific frame interval enumeration."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'driver/ov8858_sm/ov8858_sm.c').read_text(encoding='utf-8')
start = source.index('static int ov8858_sm_enum_frame_interval(')
end = source.index('\n}', start) + 2
prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
#define OV8858_SM_PIXEL_RATE 144000000LL
#define MEDIA_BUS_FMT_SBGGR10_1X10 0x3007
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct v4l2_subdev { int x; }; struct v4l2_subdev_pad_config { int x; };
struct v4l2_fract { u32 numerator,denominator; };
struct v4l2_subdev_frame_interval_enum { u32 pad,index,width,height,code;
    struct v4l2_fract interval; u32 reserved[9]; };
struct ov8858_sm_timing { u32 hts,vts,width,height; };
static struct ov8858_sm_timing ov8858_sm_timings[]={{1928,1244,1632,1224},{1940,1236,1920,1080}};
'''
test = r'''
int main(void) {
    struct v4l2_subdev s={0}; struct v4l2_subdev_frame_interval_enum f={0};
    if (ov8858_sm_enum_frame_interval(&s,0,&f) || f.width!=1632 || f.height!=1224 ||
        f.interval.numerator!=2*1928*1244 || f.interval.denominator!=144000000) return 1;
    f.index=1;
    if (ov8858_sm_enum_frame_interval(&s,0,&f)!=-EINVAL) return 2;
    f.index=0; f.width=1920; f.height=1080;
    if (ov8858_sm_enum_frame_interval(&s,0,&f) || f.interval.numerator!=2*1940*1236) return 3;
    f.width=63; f.height=64;
    if (ov8858_sm_enum_frame_interval(&s,0,&f)!=-EINVAL) return 4;
    puts("OV8858 zero-size RKAIQ and size-specific enumeration: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ov8858-enum-') as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(prelude+source[start:end]+test, encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror', '-Wno-unused-parameter',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
