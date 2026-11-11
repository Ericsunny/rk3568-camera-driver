#!/usr/bin/env python3
"""Exercise actual OV8858 control-register mapping with an I2C substitute."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'driver/ov8858_sm/ov8858_sm.c').read_text(encoding='utf-8')
start = source.index('static int ov8858_sm_set_ctrl(')
end = source.index('\nstatic const struct v4l2_ctrl_ops', start)
function = source[start:end]
prelude = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define V4L2_CID_EXPOSURE 1
#define V4L2_CID_ANALOGUE_GAIN 2
#define V4L2_CID_VBLANK 3
#define V4L2_CID_TEST_PATTERN 4
#define V4L2_CID_HFLIP 5
#define V4L2_CID_VFLIP 6
#define OV8858_SM_EXPOSURE_MIN 1
#define OV8858_SM_EXPOSURE_DEF 0x100
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
struct device { int unused; };
struct i2c_client { struct device dev; };
struct handler { int unused; };
struct v4l2_ctrl { struct handler *handler; int id, val; };
struct ov8858_sm { struct handler ctrl_handler; struct i2c_client *client;
    struct v4l2_ctrl *exposure; int is_1080p; unsigned test_pattern; };
struct ov8858_sm_timing { u32 height; };
static struct ov8858_sm_timing ov8858_sm_timings[] = {{1224},{1080}};
static unsigned addr[8], value[8], n;
static int active, fail_at, gets, pm_puts, exposure_max;
static int pm_runtime_get_if_in_use(struct device *dev) { (void)dev; if(active) gets++; return active; }
static void pm_runtime_put(struct device *dev) { (void)dev; pm_puts++; }
static int ov8858_sm_write_reg(struct i2c_client *c, unsigned a, unsigned v)
{ (void)c; addr[n]=a; value[n]=v; n++; return fail_at == (int)n ? -5 : 0; }
static void __v4l2_ctrl_modify_range(struct v4l2_ctrl *c, int min, int max, int step, int def)
{ (void)c; (void)min; (void)step; (void)def; exposure_max=max; }
'''
test = r'''
int main(void) {
    struct i2c_client client = {0};
    struct ov8858_sm sensor = {.client=&client};
    struct v4l2_ctrl ctrl = {.handler=&sensor.ctrl_handler};
    struct v4l2_ctrl exposure = {0}; sensor.exposure=&exposure;
    ctrl.id=V4L2_CID_EXPOSURE; ctrl.val=0x123; active=1;
    if (ov8858_sm_set_ctrl(&ctrl) || n!=3 || addr[0]!=0x3500 || value[0]!=0 ||
        addr[1]!=0x3501 || value[1]!=0x12 || addr[2]!=0x3502 || value[2]!=0x30) return 1;
    n=0; ctrl.id=V4L2_CID_ANALOGUE_GAIN; ctrl.val=0x100;
    if (ov8858_sm_set_ctrl(&ctrl) || n!=2 || value[0]!=1 || value[1]!=0) return 2;
    n=0; ctrl.id=V4L2_CID_VBLANK; ctrl.val=20;
    if (ov8858_sm_set_ctrl(&ctrl) || exposure_max!=1240 || n!=2 ||
        addr[0]!=0x380e || value[0]!=4 || addr[1]!=0x380f || value[1]!=0xdc) return 3;
    n=0; active=0; ctrl.id=V4L2_CID_EXPOSURE;
    if (ov8858_sm_set_ctrl(&ctrl) || n) return 4;
    active=1; fail_at=1; n=0;
    if (ov8858_sm_set_ctrl(&ctrl)!=-5 || n!=1 || gets!=pm_puts) return 5;
    active=0; fail_at=0; ctrl.id=V4L2_CID_HFLIP; ctrl.val=1;
    if (ov8858_sm_set_ctrl(&ctrl)) return 6;
    ctrl.val=0; if (ov8858_sm_set_ctrl(&ctrl)!=-EINVAL) return 7;
    ctrl.id=V4L2_CID_VFLIP; ctrl.val=0; if (ov8858_sm_set_ctrl(&ctrl)) return 8;
    ctrl.val=1; if (ov8858_sm_set_ctrl(&ctrl)!=-EINVAL) return 9;
    puts("OV8858 exposure, gain, VTS, PM and I2C failure: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ov8858-ctrl-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prelude + function + test, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
