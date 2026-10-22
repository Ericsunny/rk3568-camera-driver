#!/bin/sh
# switch_test.sh — 开关流循环回归:每轮一次完整 open→streamon→首帧→streamoff→close
# 用法: sh switch_test.sh <轮数> [结果目录]
# 输出: <目录>/switch.log(每轮一行 RESULT)+ 失败轮自动抓现场到 <目录>/fail_<n>/
#
# 异常策略:单轮失败 → 抓现场(dmesg 尾部 + 两张 media 拓扑 + 设备节点)→
#           下一轮继续(视为"可恢复");连续 3 轮失败 → 判致命,停止测试

N=${1:-500}
DIR=${2:-/root/reg/switch}
mkdir -p "$DIR"
LOG="$DIR/switch.log"
: > "$LOG"

DEV_ISP=/dev/video5     # imx415 @ rkisp mainpath, NV12
DEV_CIF=/dev/video0     # ov8858_sm @ cif, RAW10(BG10)

consecutive_fail=0
fail_total=0
i=1
while [ $i -le $N ]; do
    # 交替测两路:奇数轮 ISP(NV12),偶数轮 CIF(RAW10)
    if [ $((i % 2)) -eq 1 ]; then
        line=$(./reg_switch $DEV_ISP 3840 2160 NV12 15 2>&1)
    else
        line=$(./reg_switch $DEV_CIF 1632 1224 BG10 15 2>&1)
    fi
    rc=$?
    echo "$line" >> "$LOG"

    if [ $rc -ne 0 ]; then
        fail_total=$((fail_total + 1))
        consecutive_fail=$((consecutive_fail + 1))
        # —— 抓现场 ——
        F="$DIR/fail_${i}"
        mkdir -p "$F"
        dmesg | tail -60 > "$F/dmesg_tail.txt" 2>&1
        media-ctl -d /dev/media0 -p > "$F/media0_topology.txt" 2>&1
        media-ctl -d /dev/media1 -p > "$F/media1_topology.txt" 2>&1
        v4l2-ctl --list-devices > "$F/devices.txt" 2>&1
        echo "$line" > "$F/result.txt"
        echo "[FAIL] 轮 $i: $line (现场已存 $F)"
        if [ $consecutive_fail -ge 3 ]; then
            echo "[FATAL] 连续 $consecutive_fail 轮失败,停止测试(已跑 $i 轮)" | tee -a "$LOG"
            exit 2
        fi
    else
        consecutive_fail=0
    fi

    # 每 50 轮打一次进度(到 stdout,调用方可见)
    [ $((i % 50)) -eq 0 ] && echo "[进度] $i / $N (fail=$fail_total)"
    i=$((i + 1))
done
echo "[完成] $N 轮,失败 $fail_total" | tee -a "$LOG"
exit 0
