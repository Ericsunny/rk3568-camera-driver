#!/bin/sh
# repro_tryfmt.sh —— 复现「VIDIOC_TRY_FMT 改变实际模式」
#
# 用法(板上): sh /root/repro_tryfmt.sh
# 前提: /root/tryfmt_test 已编译好
#
# 现象链: TRY_FMT(1920x1080) 本应"只试改",实际却把驱动全局 is_1080p 置 1,
#         于是 G_FMT(ACTIVE) 开始报 1080p,而 rkcif 的 video 节点仍按 1632x1224
#         申请 buffer —— 传感器真按 1080p 出流,两边对不上。

SUB=/dev/v4l-subdev2
VID=/dev/video0

echo "=== 0. 基线 ==="
uname -v
v4l2-ctl -d $VID --get-fmt-video | sed -n '2,4p'
./tryfmt_test $SUB 0 | tail -3

echo ""
echo "=== 1. 只做一次 TRY_FMT(1920x1080) —— 这一步不应该产生任何副作用 ==="
./tryfmt_test -o 1920x1080 $SUB 0
echo "exit=$?"

echo ""
echo "=== 2. 之后 ACTIVE 报什么 ==="
v4l2-ctl -d $SUB --get-subdev-fmt pad=0 | sed -n '2,3p'

echo ""
echo "=== 3. 实际采集: video0 仍按 1632x1224 申请 buffer, 但 sensor 已切 1080p ==="
v4l2-ctl -d $VID --get-fmt-video | sed -n '2,4p'
timeout 30 /root/grab $VID 10 /tmp/polluted.raw 1632 1224 BG10 2>&1 | tail -4

echo ""
echo "=== 4. 恢复(再 TRY 一次 1632x1224 会把状态带回) ==="
./tryfmt_test -o 1632x1224 $SUB 0
echo "DONE"
