#!/usr/bin/env python3
"""把新表 imx415_hdr2_10bit_1920x1080_891M_regs_2lane 的 HTS 翻倍(0x0226 -> 0x044C)。
理由: DOL X2 每行要搬两倍数据, 若行时间不变则瞬时码率翻倍 -> CSI overflow。
把行时间也翻倍, 瞬时码率回到线性水平, 代价是帧率再减半(约 7fps)。
"""
import re
SRC = "/home/rk3568/rk3568/RK_SDK/kernel/drivers/media/i2c/imx415.c"
src = open(SRC, encoding="utf-8").read()
m = re.search(r"(static __maybe_unused const struct regval imx415_hdr2_10bit_1920x1080_891M_regs_2lane\[\] = \{)(.*?)(\n\};)", src, re.S)
if not m:
    print("找不到新表"); raise SystemExit(1)
body = m.group(2)
body2 = re.sub(r"\{0x3028,\s*0x26\},", "{0x3028, 0x4c},", body)
body2 = re.sub(r"\{0x3029,\s*0x02\},", "{0x3029, 0x04},", body2)
if body2 == body:
    print("HTS 已是目标值或未匹配"); raise SystemExit(0)
src = src[:m.start(2)] + body2 + src[m.end(2):]
open(SRC, "w", encoding="utf-8").write(src)
print("HTS 已改为 0x044C(1100)")
