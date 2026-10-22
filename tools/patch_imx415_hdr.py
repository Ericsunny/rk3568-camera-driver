#!/usr/bin/env python3
"""向 imx415.c 注入 2-lane DOL X2 (HDR_X2) 1080p 模式, 幂等。

做法(数据驱动, 不猜寄存器语义):
  以 imx415_linear_10bit_1920x1080_891M_regs_2lane 为基底(它已经包含本板 2-lane/891M/PLL/裁切的全部正确值),
  只叠加厂商 4-lane HDR2 表里与 HDR 相关、且与链路速率无关的差异:
      0x302C=1 0x302D=1 0x30CF=1 0x3260=0   (0x3260 见文件头 V0.0X01.0X01 注释:
                                             "0x3260 should be 0x01 in normal mode, should be 0x00 in hdr mode")
  另加 0x3054/55/56 = SHR1 默认值(短曝光), 由运行时 set_ctrl 覆盖。
与链路速率相关的寄存器(0x3033/0x400C/0x4018..0x4028/0x4074)一律保持 2-lane 线性表原值, 不混入。
"""
import re, sys

SRC = "/home/rk3568/rk3568/RK_SDK/kernel/drivers/media/i2c/imx415.c"
BASE = "imx415_linear_10bit_1920x1080_891M_regs_2lane"
NEW = "imx415_hdr2_10bit_1920x1080_891M_regs_2lane"

src = open(SRC, encoding="utf-8").read()

if NEW in src:
    print("已存在, 跳过"); sys.exit(0)

# --- 1) 找到基线表, 复制成新表 ---
m = re.search(r"(static __maybe_unused const struct regval %s\[\] = \{.*?\n\};\n)" % BASE, src, re.S)
if not m:
    print("找不到基线表"); sys.exit(1)
base_txt = m.group(1)
new_txt = base_txt.replace(BASE, NEW)

# --- 2) 注入/改写 HDR 相关寄存器 ---
hdr_regs = {0x302C: 0x01, 0x302D: 0x01, 0x30CF: 0x01, 0x3260: 0x00,
            0x3054: 0x09, 0x3055: 0x00, 0x3056: 0x00}
lines = new_txt.splitlines()
out, seen = [], set()
for ln in lines:
    mm = re.match(r"\{0x([0-9A-Fa-f]{4}),\s*0x([0-9A-Fa-f]{2})\},", ln.strip())
    if mm and int(mm.group(1), 16) in hdr_regs:
        reg = int(mm.group(1), 16)
        val = hdr_regs[reg]
        seen.add(reg)
        ln = "{%#06x, %#04x}," % (reg, val)
    out.append(ln)
# 未出现的寄存器, 插到 {0x3002, 0x00} 之前(即 REG_DELAY 之前)
missing = ["{%#06x, %#04x}," % (r, v) for r, v in hdr_regs.items() if r not in seen]
new_txt = "\n".join(out)
anchor = "\t{0x3002, 0x00},"
if missing:
    assert anchor in new_txt, "找不到插入锚点 {0x3002, 0x00}"
    new_txt = new_txt.replace(anchor, "\n".join(missing) + "\n" + anchor, 1)

# --- 3) 插到基线表之后 ---
src = src.replace(base_txt, base_txt + "\n" + new_txt, 1)

# --- 4) 在 supported_modes_2lane[] 末尾追加模式条目 ---
mode_entry = """	{
		/* DOL 2-frame, 2-lane, 891M/lane: 帧率减半(约 15fps), 出 2 个 VC */
		.bus_fmt = MEDIA_BUS_FMT_SGBRG10_1X10,
		.width = 1944,
		.height = 1096,
		.max_fps = {
			.numerator = 10000,
			.denominator = 150000,
		},
		.exp_def = 0x08ca - 0x08,
		.hts_def = 0x0800 * 2,
		.vts_def = 0x08ca,
		.global_reg_list = NULL,
		.reg_list = imx415_hdr2_10bit_1920x1080_891M_regs_2lane,
		.hdr_mode = HDR_X2,
		.mipi_freq_idx = 1,
		.bpp = 10,
		.vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
		.vc[PAD1] = V4L2_MBUS_CSI2_CHANNEL_1,
		.xvclk = IMX415_XVCLK_FREQ_37M,
	},
};
"""
m2 = re.search(r"(static const struct imx415_mode supported_modes_2lane\[\] = \{.*?)\n\};\n", src, re.S)
if not m2:
    print("找不到 supported_modes_2lane"); sys.exit(1)
src = src.replace(m2.group(0), m2.group(1) + "\n" + mode_entry, 1)

open(SRC, "w", encoding="utf-8").write(src)
print("已注入: %s + HDR_X2 模式条目" % NEW)
