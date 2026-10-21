#!/bin/bash
# 补测两臂 CPU(与配对实验同条件),并记录末帧数据
set -u
CTL="v4l2-ctl -d /dev/v4l-subdev6"
reset_sensor() { $CTL --set-ctrl exposure=300 >/dev/null 2>&1; $CTL --set-ctrl analogue_gain=4 >/dev/null 2>&1; }

pkill -f rkaiq_3A_server 2>/dev/null; sleep 1
reset_sensor
/usr/bin/rkaiq_3A_server > /root/ab/rk_cpu.log 2>&1 &
sleep 3
PID=$(pidof rkaiq_3A_server | awk '{print $1}')
read u0 s0 <<< $(awk '{print $14, $15}' /proc/$PID/stat)
stdbuf -oL /root/grab /dev/video5 180 /root/ab/A_cpu.nv12 3840 2160 NV12 2>&1 | tail -2
read u1 s1 <<< $(awk '{print $14, $15}' /proc/$PID/stat)
echo "  rkaiq CPU: $(( (u1-u0)+(s1-s0) )) ticks = $(echo "scale=3; (($u1-$u0)+($s1-$s0))/100" | bc) s / 180 帧 = $(echo "scale=2; ((($u1-$u0)+($s1-$s0))/100)*1000/180" | bc) ms/帧"
pkill -f rkaiq_3A_server; sleep 1

reset_sensor
/usr/bin/time -f "  mini3a CPU: user=%U sys=%S elapsed=%e" \
    /root/mini3a/mini3a -d /dev/video5 -s /dev/v4l-subdev6 -w 3840 -h 2160 -n 400 \
    > /root/ab/m3_cpu.log 2>/root/ab/m3_cpu.time
tail -1 /root/ab/m3_cpu.time
grep -E "最终" /root/ab/m3_cpu.log
echo "  130 帧对应的 CPU 见上(user+sys)/130"
echo OK
