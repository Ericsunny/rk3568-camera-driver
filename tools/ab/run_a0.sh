#!/bin/bash
# A0 臂:无任何 3A(ISP 默认参数)—— 基线偏色
set -u
/etc/init.d/rkaiq_3A.sh stop >/dev/null 2>&1
pkill -f rkaiq_3A_server 2>/dev/null
sleep 1
v4l2-ctl -d /dev/v4l-subdev6 --set-ctrl exposure=300    >/dev/null 2>&1
v4l2-ctl -d /dev/v4l-subdev6 --set-ctrl analogue_gain=4 >/dev/null 2>&1
sleep 0.5
echo "起点: $(v4l2-ctl -d /dev/v4l-subdev6 --get-ctrl exposure,analogue_gain | tr '\n' ' ')"
/root/grab /dev/video5 180 /root/ab/A0.nv12 3840 2160 NV12 2>&1 | tail -8
echo OK
