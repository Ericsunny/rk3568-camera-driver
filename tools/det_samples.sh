#!/bin/sh
# M7 固定样本集采集:用法  sh /root/det_samples.sh <label>
# 每个样本存到 /root/det_samples/<label>/
#   raw.nv12      原始 NV12(诊断用)
#   input.rgb     送进 NPU 的 640x640 RGB(模型输入图)
#   overlay.ppm   带框的叠加图
#   metrics.json  指标
#   det.txt       每帧检出的类别/分数/坐标
L="$1"
[ -z "$L" ] && { echo "用法: sh det_samples.sh <label>"; exit 2; }
cd /root || exit 1
D="/root/det_samples/$L"
mkdir -p "$D"
pkill -9 -f "camd " 2>/dev/null
sleep 1

v4l2-ctl -d /dev/v4l-subdev6 --set-ctrl exposure=300   >/dev/null 2>&1
v4l2-ctl -d /dev/v4l-subdev6 --set-ctrl analogue_gain=64 >/dev/null 2>&1
sleep 1

/root/grab /dev/video5 5 "$D/raw.nv12" 3840 2160 > "$D/grab.log" 2>&1

: > /root/camd_stage.log
./camd -d /dev/video5 -f NV12 -w 3840 -h 2160 \
       --rknn /root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn \
       --no-obj-mul --draw "$D/overlay.ppm" --dump-rgb "$D/input.rgb" \
       -b 8 --rt-depth 3 --ev-depth 2 -n 20 --json "$D/metrics.json" \
       > "$D/run.log" 2>&1
echo "rc=$?" >> "$D/run.log"

grep det= /root/camd_stage.log > "$D/det.txt" 2>/dev/null

FR=$(grep -c "det=" "$D/det.txt" 2>/dev/null)
HI=$(grep -c "det=[1-9]" "$D/det.txt" 2>/dev/null)
echo "=== 样本: $L ==="
echo "有检出的帧: ${HI:-0} / ${FR:-0}"
echo "--- 各类别出现次数(按帧计) ---"
grep -o "cls=[0-9]*" "$D/det.txt" 2>/dev/null | sort | uniq -c | sort -rn | head -n 8
echo "--- 最后一帧 ---"
tail -n 1 "$D/det.txt" 2>/dev/null
echo "--- 指标 ---"
grep -o '"npu": {[^}]*}' "$D/metrics.json" 2>/dev/null | tail -n 1
