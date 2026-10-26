#!/bin/sh
# run_3a_stack.sh —— "3A 常驻 + 事件录像 + RTSP" 一键栈(生命周期矩阵的日常形态)
#
# 前提与红线(见 docs/29 ISP 生命周期矩阵节):
#   - 3A 必须 active; 若没跑会自动 start(空闲态 start 已实测安全);
#   - ★ 绝不在采流中 stop/kill rkaiq_3A —— 会话结束后 STREAMOFF 超时,
#     ISP wedge, 新打开者全部失败, 唯一恢复=重启;
#   - 本脚本结束只 SIGTERM evrec, 不碰 3A。
#
# 用法: sh run_3a_stack.sh [时长秒, 默认 600] [结果目录, 默认 /root/stack3a]
# 产物: <目录>/evrec.log + ev_*.h264/.json + samples.csv(60s 一行)

SECS=${1:-600}
DIR=${2:-/root/stack3a}
INTERVAL=60

mkdir -p "$DIR"
: > "$DIR/samples.csv"

echo "== 3A 状态检查 =="
if [ "$(systemctl is-active rkaiq_3A)" != "active" ]; then
	echo "3A 未运行, start(空闲态 start 已实测安全)..."
	systemctl start rkaiq_3A
	sleep 2
fi
[ "$(systemctl is-active rkaiq_3A)" = "active" ] || { echo "3A 起不来, 退出"; exit 1; }
echo "3A active ✓"

rm -f /tmp/stack3a.trig
nohup /root/evrec -d /dev/video5 -w 1920 -h 1080 \
	--pre 2 --post 1 --ring-mb 64 --kbps 4000 \
	-o "$DIR/ev" --trigger /tmp/stack3a.trig \
	--rtsp 8554 > "$DIR/evrec.log" 2>&1 &
EPID=$!
echo "# evrec pid=$EPID 3A=active start=$(date +%s) secs=$SECS" >> "$DIR/samples.csv"

( while kill -0 "$EPID" 2>/dev/null; do
	touch /tmp/stack3a.trig
	sleep 90
  done ) &
TPID=$!

i=0
SCOUNT=0
while [ "$i" -lt "$SECS" ] && kill -0 "$EPID" 2>/dev/null; do
	RSS=$(grep VmRSS "/proc/$EPID/status" 2>/dev/null | awk '{print $2}')
	FD=$(ls "/proc/$EPID/fd" 2>/dev/null | wc -l)
	EV=$(ls "$DIR"/ev_*.h264 2>/dev/null | wc -l)
	RTSP=$(grep "rtsp:" "$DIR/evrec.log" | tail -1)
	echo "$(date +%s),$i,${RSS:-NA},${FD:-NA},${EV},${RTSP}" >> "$DIR/samples.csv"
	SCOUNT=$((SCOUNT + 1))
	sleep "$INTERVAL"
	i=$((i + INTERVAL))
done

kill "$TPID" 2>/dev/null
kill -TERM "$EPID" 2>/dev/null
sleep 3
echo "# stack 结束: 有效样本 $SCOUNT 行, 运行 $i 秒, 事件 $(ls "$DIR"/ev_*.h264 2>/dev/null | wc -l) 个, 3A 全程未动" >> "$DIR/samples.csv"
echo "完成: $DIR (事件 $(ls "$DIR"/ev_*.h264 2>/dev/null | wc -l) 个)"
