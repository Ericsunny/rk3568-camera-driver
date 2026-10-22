#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iq_highlight_mode.py — 在板侧切换 IMX415 IQ 的"高光优先"模式。

用法（板上执行）：
    python3 iq_highlight_mode.py status
    python3 iq_highlight_mode.py on       # OverExpCtrl.Enable = 1
    python3 iq_highlight_mode.py off      # OverExpCtrl.Enable = 0

原理：打开 ae_calib.LinearAeCtrl.OverExpCtrl.Enable 后，AE 在高光占比超过
HighLightTh(150) 时会自动压低曝光保住高光；普通场景不受影响。代价是逆光场景
整幅暗约 1.3 档（见 docs/23）。

实现要点：**文本原地改一个字符，不做 JSON 重排版**，所以开关往返后文件与原始
逐字节一致（md5 = 0666ba18ce1e4db30cddc92810587fb1，即 2022 版 IQ），不会
踩坏 tools/install_rkaiq_2022.sh 里的 md5 闸门。

生效方式：/etc/init.d/rkaiq_3A.sh stop && /etc/init.d/rkaiq_3A.sh start
（只重启 3A，不需要重启采流进程）
"""
import json
import os
import re
import sys

IQ = "/etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json"
BAK = IQ + ".bak-highlight"
ORIG_MD5 = "0666ba18ce1e4db30cddc92810587fb1"


def find_enable_line(lines):
    """返回 OverExpCtrl 块里第一行 "Enable": N, 的行号。"""
    for i, ln in enumerate(lines):
        if '"OverExpCtrl"' in ln:
            for j in range(i + 1, min(i + 30, len(lines))):
                if re.match(r'^\s*"Enable":\s*\d+\s*,?\s*$', lines[j]):
                    return j
            raise KeyError("OverExpCtrl 块里找不到 Enable 字段")
    raise KeyError("找不到 OverExpCtrl 块")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("status", "on", "off"):
        print(__doc__)
        return 2
    mode = sys.argv[1]

    with open(IQ, "r") as f:
        lines = f.readlines()

    n = find_enable_line(lines)
    cur = int(re.search(r'"Enable":\s*(\d+)', lines[n]).group(1))

    if mode == "status":
        print("iq       :", IQ)
        print("line     :", n + 1)
        print("Enable   :", cur, "(1 = 高光优先, 0 = 默认)")
        print("原文     :", lines[n].strip())
        hl = [l for l in lines[n:n + 8] if "HighLightTh" in l]
        print("HighLightTh:", hl[0].strip().split(":")[1].strip().rstrip(",") if hl else "?")
        print("备份存在 :", os.path.exists(BAK))
        return 0

    want = 1 if mode == "on" else 0
    if cur == want:
        print("已经是 %s 模式，无需修改" % ("高光优先" if want else "默认"))
        return 0

    if not os.path.exists(BAK):
        with open(BAK, "w") as f:
            f.writelines(lines)
        print("已备份 ->", BAK)

    lines[n] = re.sub(r'("Enable":\s*)\d+', r"\g<1>%d" % want, lines[n])
    tmp = IQ + ".tmp"
    with open(tmp, "w") as f:
        f.writelines(lines)

    # 自校验：JSON 仍合法，且解析出来的值确实是 want
    d = json.load(open(tmp))
    got = d["main_scene"][0]["sub_scene"][0]["scene_isp21"]["ae_calib"][
        "LinearAeCtrl"]["OverExpCtrl"]["Enable"]
    if got != want:
        os.unlink(tmp)
        print("!! 自校验失败，未替换")
        return 1
    os.replace(tmp, IQ)
    print("Enable: %d -> %d" % (cur, want))
    print("生效: /etc/init.d/rkaiq_3A.sh stop && /etc/init.d/rkaiq_3A.sh start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
