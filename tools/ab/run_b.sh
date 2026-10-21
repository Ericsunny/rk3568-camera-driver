#!/bin/bash
# 单跑 B 臂并测 CPU(bash time 取 user+sys)
set -u
SUB=/dev/v4l-subdev6
W=3840; H=2160
TIMEFORMAT='TIME user=%3U sys=%3S real=%3R'
v4l2-ctl -d $SUB --set-ctrl exposure=300    >/dev/null 2>&1
v4l2-ctl -d $SUB --set-ctrl analogue_gain=4 >/dev/null 2>&1
sleep 0.5
echo "起点: $(v4l2-ctl -d $SUB --get-ctrl exposure,analogue_gain | tr '\n' ' ')"
rm -f /root/mini3a/final.nv12
{ time /root/mini3a/mini3a -d /dev/video5 -s $SUB -w $W -h $H -n 300 \
    > /root/ab/B_mini3a.log 2>&1 ; } 2> /root/ab/B_time.log
cat /root/ab/B_time.log
grep -E "LOCKED at|>>> 最终" /root/ab/B_mini3a.log
tail -3 /root/ab/B_mini3a.log
echo OK
