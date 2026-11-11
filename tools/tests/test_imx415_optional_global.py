#!/usr/bin/env python3
"""Exercise patched BSP functions with host stubs; requires a patched imx415.c.

Usage: python3 tools/tests/test_imx415_optional_global.py /path/to/kernel/drivers/media/i2c/imx415.c
This checks control flow, not I2C, the sensor, or the target kernel.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def extract(source, name):
    found = re.search(r'static int\s+' + re.escape(name) + r'\s*\(', source)
    if not found:
        raise ValueError('function not found: ' + name)
    brace = source.index('{', found.end())
    level = 0
    for i in range(brace, len(source)):
        if source[i] == '{':
            level += 1
        elif source[i] == '}':
            level -= 1
            if level == 0:
                return source[found.start():i + 1]
    raise ValueError('unbalanced function: ' + name)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = Path(sys.argv[1]).read_text(encoding='utf-8')
    if 'Two-lane modes have no separate global table.' not in src:
        raise SystemExit('input is not the patched BSP source')
    prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
typedef uint32_t u32;
struct device { int unused; };
struct i2c_client { struct device dev; };
struct regval { unsigned int addr, val; };
struct mode { const struct regval *global_reg_list, *reg_list; int hdr_mode; };
struct imx415 { int is_thunderboot, has_init_exp, ctrl_handler, subdev, init_hdrae_exp;
                struct i2c_client *client; const struct mode *cur_mode; };
#define REG_NULL 0xffff
#define REG_DELAY 0xfffe
#define IMX415_REG_VALUE_08BIT 1
#define IMX415_REG_CTRL_MODE 0x3000
#define NO_HDR 0
#define PREISP_CMD_SET_HDRAE_EXP 1
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
static int fail_addr, writes, stream_on;
static void usleep_range(unsigned int a, unsigned int b) { (void)a; (void)b; }
static int imx415_write_reg(struct i2c_client *c, unsigned int addr, unsigned int l, unsigned int v)
{ (void)c; (void)l; (void)v; writes++; if (addr == IMX415_REG_CTRL_MODE) stream_on++;
  return addr == (unsigned int)fail_addr ? -EIO : 0; }
static int __v4l2_ctrl_handler_setup(int *h) { (void)h; return 0; }
static int imx415_ioctl(int *s, int c, void *e) { (void)s; (void)c; (void)e; return 0; }
'''
    suffix = r'''
int main(void) {
  struct i2c_client client = {0};
  struct regval global[] = {{0x100, 1}, {REG_NULL, 0}};
  struct regval mode_regs[] = {{0x200, 2}, {REG_NULL, 0}};
  struct mode m = {0, mode_regs, NO_HDR};
  struct imx415 s = {.client=&client, .cur_mode=&m};
  assert(__imx415_start_stream(&s) == 0 && writes == 2 && stream_on == 1);
  writes=stream_on=0; m.global_reg_list=global;
  assert(__imx415_start_stream(&s) == 0 && writes == 3 && stream_on == 1);
  writes=stream_on=0; m.global_reg_list=0; m.reg_list=0;
  assert(__imx415_start_stream(&s) == -EINVAL && writes == 0 && stream_on == 0);
  writes=stream_on=0; m.reg_list=mode_regs; fail_addr=0x200;
  assert(__imx415_start_stream(&s) == -EIO && writes == 1 && stream_on == 0);
  writes=stream_on=0; m.global_reg_list=global; fail_addr=0x100;
  assert(__imx415_start_stream(&s) == -EIO && writes == 1 && stream_on == 0);
  return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='imx415-control-test-') as tmp:
        harness = Path(tmp) / 'test.c'
        binary = Path(tmp) / 'test'
        harness.write_text(prefix + extract(src, 'imx415_write_array') + '\n' +
                           extract(src, '__imx415_start_stream') + suffix, encoding='utf-8')
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS optional global, required table and I2C error propagation (host stubs)')


if __name__ == '__main__':
    main()
