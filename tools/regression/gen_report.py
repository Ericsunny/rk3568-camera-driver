#!/usr/bin/env python3
# gen_report.py — 汇总回归日志生成 markdown 报告
# 用法: python3 gen_report.py <run目录>(含 switch/ soak/ suspend/)
import os, re, sys

base = sys.argv[1] if len(sys.argv) > 1 else "/root/reg"
out = []

def pct(vals, p):
    if not vals: return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(len(s) * p / 100))]

# ---------- switch ----------
sw_lines = []
for path in (os.path.join(base, "switch", "switch.log"),):
    if os.path.exists(path):
        sw_lines = [l.strip() for l in open(path) if l.startswith("RESULT")]
ok = [l for l in sw_lines if " ok " in l]
fail = [l for l in sw_lines if " fail " in l]
first_all, first_isp, first_cif, fps_all = [], [], [], []
for l in ok:
    m = re.search(r"first_ms=([0-9.]+)", l)
    f = re.search(r"fps=([0-9.]+)", l)
    if m: first_all.append(float(m.group(1)))
    if f: fps_all.append(float(f.group(1)))
    if "/dev/video5" in l: first_isp.append(float(m.group(1)) if m else 0)
    if "/dev/video0" in l: first_cif.append(float(m.group(1)) if m else 0)

out.append("# 板端回归测试报告")
out.append("")
out.append(f"运行目录:`{base}`")
out.append("")
out.append("## 1. 开关流循环")
if sw_lines:
    total = len(sw_lines)
    out.append("")
    out.append(f"| 指标 | 值 |")
    out.append(f"|---|---|")
    out.append(f"| 总轮次(交替 ISP@NV12 / CIF@BG10) | {total} |")
    out.append(f"| 成功 / 失败 | {len(ok)} / {len(fail)}(**成功率 {len(ok)*100.0/total:.2f}%**) |")
    if first_all:
        out.append(f"| 首帧耗时 min / avg / p95 / max | {min(first_all):.1f} / {sum(first_all)/len(first_all):.1f} / {pct(first_all,95):.1f} / {max(first_all):.1f} ms |")
    if first_isp and first_cif:
        out.append(f"| 首帧 avg(ISP 4K NV12) | {sum(first_isp)/len(first_isp):.1f} ms |")
        out.append(f"| 首帧 avg(CIF 1632x1224 BG10) | {sum(first_cif)/len(first_cif):.1f} ms |")
    if fps_all:
        out.append(f"| 稳态 fps min / avg | {min(fps_all):.1f} / {sum(fps_all)/len(fps_all):.1f} |")
    if fail:
        out.append("")
        out.append("### 失败明细")
        out.append("")
        out.append("| # | 结果 |")
        out.append("|---|---|")
        for i, l in enumerate(fail, 1):
            out.append(f"| {i} | `{l}` |")
        fdirs = [d for d in os.listdir(os.path.join(base, "switch")) if d.startswith("fail_")]
        if fdirs:
            out.append("")
            out.append(f"每轮失败的现场(dmesg/media 拓扑/设备表)已存 `{base}/switch/fail_*/`({len(fdirs)} 组)")
else:
    out.append("_无 switch 数据_")

# ---------- soak ----------
out.append("")
out.append("## 2. 长稳采集")
sp = os.path.join(base, "soak", "soak.log")
if os.path.exists(sp):
    last = ""
    for l in open(sp):
        if l.startswith("RESULT"): last = l.strip()
    if last:
        m = re.search(r"frames=(\d+) expected=(\d+) drop_pct=([0-9.]+) duration_s=(\d+)", last)
        if m:
            out.append("")
            out.append("| 指标 | 值 |")
            out.append("|---|---|")
            out.append(f"| 时长 | {m.group(4)} s |")
            out.append(f"| 实际帧数 / 理论 | {m.group(1)} / {m.group(2)} |")
            out.append(f"| **掉帧率** | **{m.group(3)}%** |")
        else:
            out.append(f"`{last}`")
    else:
        out.append("_soak 未完成(无 RESULT 行)_")
else:
    out.append("_无 soak 数据_")

# ---------- suspend ----------
out.append("")
out.append("## 3. 停流→suspend→resume→开流")
sl = os.path.join(base, "suspend", "suspend.log")
if os.path.exists(sl):
    oks, fails, resume_ms = [], [], []
    for l in open(sl):
        if l.startswith("RESULT ok"):
            oks.append(l.strip())
            m = re.search(r"resume_first_ms=([0-9.]+)", l)
            if m: resume_ms.append(float(m.group(1)))
        elif l.startswith("RESULT fail"):
            fails.append(l.strip())
    out.append("")
    out.append("| 指标 | 值 |")
    out.append("|---|---|")
    out.append(f"| 轮次 / 成功 / 失败 | {len(oks)+len(fails)} / {len(oks)} / {len(fails)} |")
    if resume_ms:
        out.append(f"| 恢复耗时(resume→ISP 首帧) min / avg / max | {min(resume_ms):.1f} / {sum(resume_ms)/len(resume_ms):.1f} / {max(resume_ms):.1f} ms |")
    if fails:
        out.append("")
        out.append("失败轮:")
        for l in fails: out.append(f"- `{l}`")
else:
    out.append("_无 suspend 数据(可能仍在跑或未跑)_")

# ---------- 结论模板 ----------
out.append("")
out.append("## 4. 结论(人工填)")
out.append("")
out.append("- [ ] 开关流稳定性:____")
out.append("- [ ] 长稳掉帧:____")
out.append("- [ ] suspend/resume 链路:____")
out.append("- [ ] 暴露的问题与定性:____")

print("\n".join(out))
