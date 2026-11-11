#!/usr/bin/env python3
"""Exercise actual sensor ioctl dispatch for supported linear HDR mode."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2]/'driver/ov8858_sm/ov8858_sm.c').read_text(encoding='utf-8')
start = source.index('static long ov8858_sm_ioctl(')
end = source.index('\n#ifdef CONFIG_COMPAT', start)
prelude = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define RKMODULE_GET_MODULE_INFO 1
#define RKMODULE_GET_HDR_CFG 2
#define RKMODULE_SET_HDR_CFG 3
#define RKMODULE_SET_QUICK_STREAM 4
#define NO_HDR 0
#define HDR_NORMAL_VC 0
#define dev_err(...) ((void)0)
struct rkmodule_hdr_cfg { u32 hdr_mode; struct { u32 mode; } esp; };
struct v4l2_subdev { int unused; };
struct device { int unused; }; struct i2c_client { struct device dev; };
struct ov8858_sm { struct v4l2_subdev subdev; struct i2c_client *client; };
static void ov8858_sm_get_module_inf(struct ov8858_sm *s, void *v) { (void)s; (void)v; }
static int pm_runtime_get_sync(struct device *d) { (void)d; return 0; }
static void pm_runtime_put_noidle(struct device *d) { (void)d; }
static void pm_runtime_mark_last_busy(struct device *d) { (void)d; }
static void pm_runtime_put_autosuspend(struct device *d) { (void)d; }
static int ov8858_sm_write_reg(struct i2c_client *c, int r, int v) { (void)c;(void)r;(void)v;return 0; }
'''
test = r'''
int main(void) {
    struct i2c_client c={0}; struct ov8858_sm s={.client=&c};
    struct rkmodule_hdr_cfg h={.hdr_mode=5,.esp={1}};
    if (ov8858_sm_ioctl(&s.subdev,RKMODULE_GET_HDR_CFG,&h) || h.hdr_mode!=0 || h.esp.mode!=0) return 1;
    if (ov8858_sm_ioctl(&s.subdev,RKMODULE_SET_HDR_CFG,&h)) return 2;
    h.hdr_mode=5;
    if (ov8858_sm_ioctl(&s.subdev,RKMODULE_SET_HDR_CFG,&h)!=-EINVAL) return 3;
    puts("OV8858 linear HDR ioctl and unsupported mode rejection: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ov8858-hdr-') as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(prelude+source[start:end]+test,encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
