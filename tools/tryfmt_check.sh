#!/bin/sh
# collect_tryfmt.sh <tag> —— 修复前/后统一取证
#   sh /root/collect_tryfmt.sh before
#   sh /root/collect_tryfmt.sh after
TAG=${1:-before}
OUT=/root/tryfmt_${TAG}.log
: > $OUT

{
echo "===== tryfmt $TAG ====="
echo "time: $(date)"
echo "kernel: $(uname -r) $(uname -v)"
echo "param: inject_stop_err=$(cat /sys/module/ov8858_sm/parameters/inject_stop_err 2>/dev/null)"
echo
echo "--- 1) video0 当前格式(基线) ---"
v4l2-ctl -d /dev/video0 --get-fmt-video | sed -n '2,4p'
echo
echo "--- 2) TRY sweep(8 个用例) ---"
/root/tryfmt_test /dev/v4l-subdev2 0
echo "sweep_exit=$?"
echo
echo "--- 3) 单次 TRY(1920x1080):复现「TRY 改了 ACTIVE」 ---"
/root/tryfmt_test -o 1920x1080 /dev/v4l-subdev2 0
echo "single_exit=$?"
echo
echo "--- 4) 之后 ACTIVE 报什么 ---"
v4l2-ctl -d /dev/v4l-subdev2 --get-subdev-fmt pad=0 | sed -n '2,3p'
echo
echo "--- 5) 实际采集(video0 仍是 1632x1224) ---"
v4l2-ctl -d /dev/video0 --get-fmt-video | sed -n '2,4p'
rm -f /tmp/t.raw
timeout 30 /root/grab /dev/video0 10 /tmp/t.raw 1632 1224 BG10 2>&1 | tail -2
echo "saved_bytes=$(stat -c %s /tmp/t.raw 2>/dev/null)  (1632x1224 应为 2048*1224=2506752;若为 2048*1080=2211840 说明 sensor 按 1080p 出流)"
echo
echo "--- 6) 恢复 ---"
/root/tryfmt_test -o 1632x1224 /dev/v4l-subdev2 0 | tail -2
echo
echo "--- 7) 停流错误是否向上返回(fault-inject) ---"
echo "current inject_stop_err=$(cat /sys/module/ov8858_sm/parameters/inject_stop_err 2>/dev/null)"
echo "见 /root/streamoff_err_${TAG}.log"
} >> $OUT 2>&1

echo "written: $OUT"
cat $OUT
