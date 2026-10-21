#!/bin/bash
# 配对 A/B:同一分钟内交替跑 rkaiq 臂与 mini3a 臂,各 2 轮
set -u
mkdir -p /root/ab
CTL="v4l2-ctl -d /dev/v4l-subdev6"

reset_sensor() {
    $CTL --set-ctrl exposure=300 >/dev/null 2>&1
    $CTL --set-ctrl analogue_gain=4 >/dev/null 2>&1
}

echo "===== 场景基线(300/4,无 3A) ====="
pkill -f rkaiq_3A_server 2>/dev/null; sleep 1
reset_sensor
stdbuf -oL /root/grab /dev/video5 60 /root/ab/base.nv12 3840 2160 NV12 2>&1 | grep -E "帧  (0|30)|抓" | sed 's/^/  /'

for i in 1 2; do
    echo
    echo "===== 第 $i 轮 · A 臂 rkaiq ====="
    pkill -f rkaiq_3A_server 2>/dev/null; sleep 1
    reset_sensor
    /usr/bin/rkaiq_3A_server > /root/ab/rk_${i}.log 2>&1 &
    sleep 3
    stdbuf -oL /root/grab /dev/video5 180 /root/ab/A_${i}.nv12 3840 2160 NV12 2>&1 \
        | grep -E "帧 |抓" | sed 's/^/  /'
    echo -n "  sensor 终值: "; $CTL --get-ctrl exposure,analogue_gain | tr '\n' ' '; echo
    pkill -f rkaiq_3A_server 2>/dev/null; sleep 1

    echo "===== 第 $i 轮 · B 臂 mini3a ====="
    reset_sensor
    ( cd /root/mini3a && timeout 60 ./mini3a -d /dev/video5 -s /dev/v4l-subdev6 \
        -w 3840 -h 2160 -n 400 ) > /root/ab/m3_${i}.log 2>&1
    grep -E "LOCKED|最终" /root/ab/m3_${i}.log | sed 's/^/  /'
    echo -n "  sensor 终值: "; $CTL --get-ctrl exposure,analogue_gain | tr '\n' ' '; echo
done
echo
echo "===== 结束,确认无残留 3A 进程 ====="
pidof rkaiq_3A_server || echo "  无 rkaiq 进程"
echo OK
