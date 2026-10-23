#!/usr/bin/env python3
"""det_recount.py —— 按"每一帧推理"重算检出率(零检出帧计入分母)。

旧统计只累加 det.txt 里出现的框,没有逐帧口径;本脚本把每个
`rt seq=` 行都当作一帧,缺失/越界都计入分母。

用法: det_recount.py <thr> <dir-or-txt>...
输出: 每个样本 帧数 / 各类命中帧数 / 命中率 / 分数统计
"""
import re, sys, glob, os

LINE = re.compile(r"rt seq=(\d+)\s+det=(\d+).*?max=([\d.]+)")
BOX = re.compile(r"\[(\d+)\]cls=(\d+) s=([\d.]+)")

CLS = 0

def load(fn, thr):
    frames = cls_hit = 0
    best = []
    with open(fn) as f:
        for line in f:
            if not line.startswith("rt seq="):
                continue
            frames += 1
            sc = [float(m.group(3)) for m in BOX.finditer(line)
                  if int(m.group(2)) == CLS]
            sc = [v for v in sc if v >= thr]
            if sc:
                cls_hit += 1
                best.append(max(sc))
    return frames, cls_hit, best

def main():
    thr = float(sys.argv[1])
    targets = []
    for a in sys.argv[2:]:
        targets += [a] if os.path.isfile(a) else sorted(glob.glob(os.path.join(a, "*/det.txt")))
    for fn in targets:
        name = os.path.basename(os.path.dirname(fn))
        n, hit, sc = load(fn, thr)
        rate = 100.0 * hit / n if n else 0.0
        s = "thr=%.2f %-12s frames=%2d hit=%2d rate=%5.1f%%" % (thr, name, n, hit, rate)
        if sc:
            s += " score min/avg/max=%.2f/%.2f/%.2f" % (min(sc), sum(sc) / len(sc), max(sc))
        print(s)

main()
