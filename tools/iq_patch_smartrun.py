#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
iq_patch_smartrun.py -- 给 Rockchip RK AIQ 的 IQ json 补上 smartRun 校准块

问题
----
RK3568 上 AIQ 6.0 (release 2024-03-03) 的 librkaiq.so 要求 AWB 校准里必须有
    wb_v21.autoExtPara.smartRun.cfg
否则 AWB 预处理会带着空数组去插值:

    AWB:E:lost smartRun para in json
    Thread 9 "xc:GrpMsgThrd" received signal SIGSEGV
    #0 interpolation(float const*, float const*, int, float, float*)
    #1 AwbPreProcV201(rk_aiq_awb_stat_res_v201_s, awb_contex_s*)

gdb 里看得很清楚: x0=x1=NULL, len=0 -> 必崩。
2022 版 IQ 文件(库里 36 个 json 全部如此)没有这个字段, 于是"库比 IQ 新"就崩。

结构(踩坑记录, 别乱改)
----------------------
* smartRun 是 autoExtPara 的**兄弟字段**, 不是 autoPara 的;
* smartRun.cfg 是一个**对象**, 不是数组(写成数组 -> enable 读不到, 长度全 0);
* cfg 里是 offset + 4 组 {数组, 对应 _len};

    "smartRun": {
      "enable": 0,
      "cfg": {
        "offset": 0,
        "lumaValue": [...],       "lumaValue_len": 4,
        "lvVarTh": [...],         "lvVarTh_len": 4,
        "wbgainAlgDiffTh": [...], "wbgainAlgDiffTh_len": 4,
        "wbgainHwDiffTh": [...],  "wbgainHwDiffTh_len": 4
      }
    }

* 少一个 _len      -> "E:check _len para in smartRun.cfg in json"
* 四个长度不一致或全 0 -> "E:lost smartRun para in json"
* 四个长度必须相等且非 0, 否则 UpdateAwbCalibV2Para() 直接 return,
  后面的 JsonPara2SwParaCom() 不执行, 策略里数组仍是 NULL -> 段错误。

用法
----
    python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json            # 补 enable=0(最保守)
    python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json --enable 1 # 真开 smartRun
    python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json --check    # 只检查不改

脚本按文本插入, 保留原文件排版; 重复执行是幂等的。
"""

from __future__ import annotations

import argparse
import json
import re
import sys

# smartRun.cfg 的四个数组必须等长且非 0, 这里用 4 个点(库内部默认表也是 4 个点)
LUMA_VALUE = [0.0, 1000.0, 3000.0, 5000.0]
LV_VAR_TH = [0.9, 0.05, 0.7, 0.04]
WBGAIN_ALG_DIFF_TH = [0.1, 0.1, 0.1, 0.1]
WBGAIN_HW_DIFF_TH = [0.1, 0.1, 0.1, 0.1]

ANCHOR = '"autoExtPara": {'


def build_block(enable: int, indent: str) -> str:
    """按 autoExtPara 子字段的缩进拼出 smartRun 文本块。"""
    i1 = indent                    # "smartRun" 的缩进 = autoExtPara 子字段缩进
    i2 = indent + "  "
    i3 = indent + "    "
    vals = [
        ("lumaValue", LUMA_VALUE),
        ("lvVarTh", LV_VAR_TH),
        ("wbgainAlgDiffTh", WBGAIN_ALG_DIFF_TH),
        ("wbgainHwDiffTh", WBGAIN_HW_DIFF_TH),
    ]
    lines = [
        f'{i1}"smartRun": {{',
        f'{i1}  "enable": {enable},',
        f'{i1}  "cfg": {{',
        f'{i2}  "offset": 0,',
    ]
    for name, arr in vals:
        body = ", ".join(f"{v:.4g}" for v in arr)
        lines.append(f'{i2}  "{name}": [{body}], "{name}_len": {len(arr)},')
    # 去掉最后一行的逗号, 保持合法 json
    lines[-1] = lines[-1].rstrip(",")
    lines.append(f'{i2}}}')
    lines.append(f'{i1}}},')
    return "\n".join(lines) + "\n"


def find_wb_v21(obj):
    """返回 (main_scene[0].sub_scene[0].scene_isp21.wb_v21) 这个 dict, 找不到返回 None。"""
    try:
        return obj["main_scene"][0]["sub_scene"][0]["scene_isp21"]["wb_v21"]
    except (KeyError, IndexError, TypeError):
        return None


def has_smartrun(wb) -> bool:
    ext = wb.get("autoExtPara") if isinstance(wb, dict) else None
    return isinstance(ext, dict) and "smartRun" in ext


def patch(path: str, enable: int, check_only: bool) -> int:
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    # 先确认这是一份能被解析、且结构对得上的 IQ 文件
    try:
        obj = json.loads(src)
    except json.JSONDecodeError as e:
        print(f"[x] {path} 不是合法 json: {e}", file=sys.stderr)
        return 2

    wb = find_wb_v21(obj)
    if wb is None:
        print(f"[x] {path} 里找不到 main_scene[0].sub_scene[0].scene_isp21.wb_v21",
              file=sys.stderr)
        return 3

    if check_only:
        ok = has_smartrun(wb)
        ext = wb.get("autoExtPara", {})
        sr = ext.get("smartRun") if isinstance(ext, dict) else None
        print(f"[i] smartRun 存在: {ok}")
        if ok:
            cfg = sr.get("cfg") if isinstance(sr, dict) else None
            print(f"[i] enable     = {sr.get('enable')}")
            print(f"[i] cfg 类型   = {type(cfg).__name__}"
                  f"{'(正确)' if isinstance(cfg, dict) else '(应为对象!)'}")
            if isinstance(cfg, dict):
                for name in ("lumaValue", "lvVarTh",
                             "wbgainAlgDiffTh", "wbgainHwDiffTh"):
                    print(f"[i] {name}_len = {cfg.get(name + '_len')}")
        return 0 if ok else 1

    if has_smartrun(wb):
        print(f"[=] {path} 已有 smartRun, 不重复插入(要改 enable 请先手工删掉)")
        return 0

    # 文本插入: 找到 autoExtPara 那一行, 按它的缩进生成子块
    m = re.search(r"^([ \t]*)" + re.escape(ANCHOR), src, flags=re.MULTILINE)
    if not m:
        print(f"[x] 文本里找不到锚点 {ANCHOR}", file=sys.stderr)
        return 4

    # 该文件里可能有多处 autoExtPara, 必须唯一, 否则容易插错相机
    if len(re.findall(r"^[ \t]*" + re.escape(ANCHOR), src, flags=re.MULTILINE)) != 1:
        print(f"[x] {ANCHOR} 出现多次, 拒绝盲目插入", file=sys.stderr)
        return 5

    parent_indent = m.group(1)
    child_indent = parent_indent + "  "
    block = build_block(enable, child_indent)

    insert_at = m.end()          # 插在 "autoExtPara": { 之后
    new_src = src[:insert_at] + "\n" + block.rstrip("\n") + src[insert_at:]

    # 插完必须仍然是合法 json, 且四个长度能被读出来
    try:
        obj2 = json.loads(new_src)
    except json.JSONDecodeError as e:
        print(f"[x] 插入后 json 非法({e}), 放弃写入", file=sys.stderr)
        return 6

    sr = find_wb_v21(obj2)["autoExtPara"]["smartRun"]
    lens = [sr["cfg"][n + "_len"] for n in
            ("lumaValue", "lvVarTh", "wbgainAlgDiffTh", "wbgainHwDiffTh")]
    if len(set(lens)) != 1 or lens[0] == 0:
        print(f"[x] 长度不合法: {lens}", file=sys.stderr)
        return 7

    with open(path, "w", encoding="utf-8") as f:
        f.write(new_src)

    print(f"[+] {path}")
    print(f"    smartRun.enable = {enable}, cfg 四个数组长度 = {lens}")
    print(f"    文件从 {len(src)} 字节变为 {len(new_src)} 字节 "
          f"(+{len(new_src) - len(src)})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="给 RK AIQ IQ json 补 smartRun 校准块")
    ap.add_argument("iqfile", help="IQ json 路径, 例如 /etc/iqfiles/imx415_xxx.json")
    ap.add_argument("--enable", type=int, choices=(0, 1), default=0,
                    help="smartRun.enable, 默认 0(只补字段不改变 AWB 行为)")
    ap.add_argument("--check", action="store_true", help="只检查, 不修改")
    args = ap.parse_args()
    return patch(args.iqfile, args.enable, args.check)


if __name__ == "__main__":
    sys.exit(main())
