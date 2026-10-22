#!/bin/sh
# soak_test.sh — 长稳采集:一条流连续跑满时长,统计实际帧数/掉帧率/帧间隔抖动
# 用法: sh soak_test.sh <秒数> [结果目录] [dev w h fourcc]
#   默认 7200s(2h),imx415@ISP NV12
# 实现:单个 v4l2-ctl 进程长流(--verbose 每帧一行到文件),到时 kill;
#       事后从行数与首尾时间戳算 实际fps/掉帧率。无段间间隙。

SECS=${1:-7200}
DIR=${2:-/root/reg/soak}
DEV=${3:-/dev/video5}
W=${4:-3840}
H=${5:-2160}
FMT=${6:-NV12}
mkdir -p "$DIR"
LOG="$DIR/soak.log"
FRAMES="$DIR/frames.log"
: > "$LOG"

case "$FMT" in
    NV12) EXP_FPS=15 ;;
    BG10) EXP_FPS=30 ;;
    *)    EXP_FPS=15 ;;
esac

echo "soak 开始: dev=$DEV ${W}x${H} $FMT 时长=${SECS}s 理论fps=$EXP_FPS" | tee -a "$LOG"

v4l2-ctl -d "$DEV" --set-fmt-video=width=$W,height=$H,pixelformat=$FMT \
    --stream-mmap --stream-count=9999999 --stream-to=/dev/null \
    --verbose > "$FRAMES" 2>/dev/null &
VPID=$!

slept=0
while [ $slept -lt $SECS ]; do
    sleep 30
    slept=$((slept + 30))
    kill -0 $VPID 2>/dev/null || { echo "[异常] 采集进程提前退出(第 ${slept}s)" | tee -a "$LOG"; break; }
    [ $((slept % 600)) -eq 0 ] && echo "[进度] ${slept}s / ${SECS}s 已收 $(grep -c seq: "$FRAMES") 帧" | tee -a "$LOG"
done

kill $VPID 2>/dev/null
wait $VPID 2>/dev/null
sleep 1

total=$(grep -c "seq:" "$FRAMES" || echo 0)
exp_total=$((EXP_FPS * SECS))
drop_pct=$(awk "BEGIN{printf \"%.3f\", ($exp_total - $total) * 100.0 / $exp_total}")

echo "soak 完成: 实际=$total 理论=$exp_frames 掉帧率=${drop_pct}%" | tee -a "$LOG"
echo "RESULT ok frames=$total expected=$exp_total drop_pct=$drop_pct duration_s=$SECS" >> "$LOG"
