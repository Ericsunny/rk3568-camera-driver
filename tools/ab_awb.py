#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ab_awb.py -- 同场景对比 rkaiq AWB 与 mini3a AWB

三个 arm, 同一台板、同一个镜头、同一盏灯、同一个分辨率(NV12 3840x2160):

  A. 基线   : 不跑 3A, 只 grab(用来看"没人管白平衡时偏多少")
  B. rkaiq  : rkaiq_3A_server 做 AE+AWB(修过 smartRun 的 IQ), 同时 grab 统计
  C. mini3a : 自己写的 AE/AWB 闭环(isp_wb.ko 下发数字增益)

指标: 末帧 Y、U/V 相对 128 的偏移、收敛到 |偏移|<=3 的帧号、最后 5 个采样点的抖动。
"""

import os
import re
import signal
import subprocess
import sys
import time

DEV = "/dev/video5"
W, H = 3840, 2160
GRAB = "/root/grab"
MINI3A = "/root/mini3a/mini3a"

GRAB_RE = re.compile(
    r"帧\s*(\d+):\s*Y\s*([\d.]+)\s+U\s*([\d.]+)\(\s*([-+]?[\d.]+)\)"
    r"\s+V\s*([\d.]+)\(\s*([-+]?[\d.]+)\)")
M3_RE = re.compile(r"f=(\d+)\s+Y=(\d+)\s+U=(\d+)\s+V=(\d+)")

results = []


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True,
                          text=True, timeout=timeout)


def grab(n):
    r = sh([GRAB, DEV, str(n), "/tmp/ab_grab.raw", str(W), str(H), "NV12"],
           timeout=240)
    pts = []
    for m in GRAB_RE.finditer(r.stdout):
        pts.append({"f": int(m.group(1)), "y": float(m.group(2)),
                    "du": float(m.group(4)), "dv": float(m.group(6))})
    return pts


def summarize(name, pts, extra=""):
    if not pts:
        print(f"{name}: 没解析到数据")
        results.append((name, None))
        return
    last = pts[-1]
    conv = next((p["f"] for p in pts
                 if abs(p["du"]) <= 3 and abs(p["dv"]) <= 3), None)
    tail = pts[-5:]
    jitter = max(max(abs(p["du"]) for p in tail), max(abs(p["dv"]) for p in tail))
    print(f"\n[{name}]")
    print(f"  末帧 Y={last['y']:.1f}   U偏移={last['du']:+.1f}   V偏移={last['dv']:+.1f}")
    print(f"  |偏移|<=3 的收敛帧: {conv if conv is not None else '未收敛'}")
    print(f"  最后 5 个采样点最大|偏移| = {jitter:.1f} {extra}")
    results.append((name, last, conv, jitter))


def main():
    print("=== A/B: rkaiq AWB vs mini3a AWB ===")
    print(f"设备 {DEV}  {W}x{H} NV12")

    # 清场
    sh("pkill -f rkaiq_3A_server || true")
    sh("pkill -f mini3a || true")
    time.sleep(1)

    # A. 基线
    print("\n--- A. 基线(无 3A)---")
    summarize("基线 无3A", grab(120))

    # B. rkaiq
    print("\n--- B. rkaiq(AE+AWB)---")
    log = open("/tmp/ab_rkaiq.log", "w")
    p = subprocess.Popen(["/usr/bin/rkaiq_3A_server"], stdout=log,
                         stderr=subprocess.STDOUT)
    try:
        time.sleep(3)
        rk = grab(300)
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        log.close()
    summarize("rkaiq AWB", rk, f"(日志 /tmp/ab_rkaiq.log)")

    # C. mini3a
    print("\n--- C. mini3a(AE+AWB)---")
    m3pts, m3txt = [], ""
    try:
        r = sh([MINI3A, "-d", DEV, "-w", str(W), "-h", str(H)], timeout=90)
        m3txt = r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        m3txt = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    open("/tmp/ab_mini3a.log", "w").write(m3txt)
    for m in M3_RE.finditer(m3txt):
        f, y, u, v = (int(m.group(i)) for i in range(1, 5))
        m3pts.append({"f": f, "y": float(y), "du": u - 128.0, "dv": v - 128.0})
    locked = [ln for ln in m3txt.splitlines() if "LOCKED" in ln or "增益" in ln]
    summarize("mini3a AWB", m3pts, f"({len(locked)} 条锁定/增益行)")
    for ln in locked[-3:]:
        print("   ", ln.strip())

    print("\n=== 汇总 ===")
    print(f"{'方案':<14}{'Y':>7}{'U偏移':>9}{'V偏移':>9}{'收敛帧':>9}")
    for item in results:
        if len(item) == 2:
            print(f"{item[0]:<14}   n/a")
            continue
        name, last, conv, jitter = item
        print(f"{name:<14}{last['y']:>7.1f}{last['du']:>9.1f}"
              f"{last['dv']:>9.1f}{str(conv):>9}")
    print("\n原始日志: /tmp/ab_rkaiq.log  /tmp/ab_mini3a.log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
