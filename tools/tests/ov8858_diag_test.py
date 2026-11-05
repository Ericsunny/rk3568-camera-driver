#!/usr/bin/env python3
"""Host test for the driver's actual diagnostic read wrapper."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] /
          "driver/ov8858_sm/ov8858_sm.c").read_text(encoding="utf-8")
signature = "static int ov8858_sm_read_diag("
start = source.rindex(signature)
opening = source.index("{", start)
depth = 0
for pos in range(opening, len(source)):
    if source[pos] == "{":
        depth += 1
    elif source[pos] == "}":
        depth -= 1
        if not depth:
            helper = source[start:pos + 1]
            break
else:
    raise RuntimeError("diagnostic helper not found")

assert source.count("if (!diag_err)") == 2
assert source.count("diag_err |= ov8858_sm_read_diag(") == 21
program = r'''
#include <stdio.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
struct device { int unused; };
struct i2c_client { struct device dev; };
static int read_rc, warnings;
static int ov8858_sm_read_reg(struct i2c_client *c, u16 reg, u8 *value)
{ (void)c; (void)reg; if (read_rc) return read_rc; *value = 0; return 0; }
#define dev_warn(dev, fmt, ...) (warnings++)
''' + helper + r'''
int main(void)
{
    struct i2c_client c = {0};
    u8 value = 42;
    read_rc = 0;
    if (ov8858_sm_read_diag(&c, 0x3501, &value) || value != 0 || warnings) return 1;
    value = 42; read_rc = -5;
    if (ov8858_sm_read_diag(&c, 0x3501, &value) != -5 || value != 42 || warnings != 1) return 2;
    puts("diagnostic real zero vs read failure: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ov8858-diag-") as tmp:
    cfile = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    cfile.write_text(program, encoding="utf-8")
    subprocess.run(["cc", "-Wall", "-Werror", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
