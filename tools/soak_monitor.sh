#!/bin/sh
# soak_monitor.sh —— 当前服务(evrec 长跑)的稳定性采样:RSS/线程/fd/温度/事件数
#
# 用法: sh soak_monitor.sh <秒数> <结果目录>
#   默认 7200s(2h);24h 传 86400。建议 2h 通过后再跑 24h。
#
# 被测服务: evrec 1080p + RTSP 8554(无客户端,服务端全链采集/编码/入环),
#           每 90s 写一次触发文件产生一个事件(含 pre/post 落盘)。
# 采样: 每 60s 一行 CSV: unix秒,采样序,RSS_kb,线程数,fd数,温度_毫摄氏度,已完成事件数
# 判读: RSS 单调增长 / fd 增长 = 泄漏; 温度持续升 = 散热问题; 事件数 = 帧率佐证。
#
# 结束后: kill -TERM 会让 evrec 优雅退出(进行中事件落盘)。

SECS=${1:-7200}
DIR=${2:-/root/soak}
EVREC=/root/evrec
INTERVAL=60

mkdir -p "$DIR"
: > "$DIR/samples.csv"
rm -f /tmp/soak.trigger

# 被测服务:输出前缀 ev_<启动时间>,事件文件 ev_<ts>-NN.h264/.json
nohup "$EVREC" -d /dev/video5 -w 1920 -h 1080 \
	--pre 2 --post 1 --ring-mb 64 --kbps 4000 \
	-o "$DIR/ev" --trigger /tmp/soak.trigger \
	--rtsp 8554 > "$DIR/evrec.log" 2>&1 &
EPID=$!
echo "# evrec pid=$EPID start=$(date +%s) secs=$SECS" >> "$DIR/samples.csv"

# 事件节拍(独立进程,服务退出后自灭)
( while kill -0 "$EPID" 2>/dev/null; do
	touch /tmp/soak.trigger
	sleep 90
  done ) &
TPID=$!

i=0
SCOUNT=0
while [ "$i" -lt "$SECS" ] && kill -0 "$EPID" 2>/dev/null; do
	RSS=$(grep VmRSS "/proc/$EPID/status" 2>/dev/null | awk '{print $2}')
	THR=$(grep Threads "/proc/$EPID/status" 2>/dev/null | awk '{print $2}')
	FD=$(ls "/proc/$EPID/fd" 2>/dev/null | wc -l)
	TMP=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
	EV=$(ls "$DIR"/ev_*.h264 2>/dev/null | wc -l)
	echo "$(date +%s),$i,${RSS:-NA},${THR:-NA},${FD:-NA},${TMP:-NA},${EV}" >> "$DIR/samples.csv"
	SCOUNT=$((SCOUNT + 1))
	sleep "$INTERVAL"
	i=$((i + INTERVAL))
done

kill "$TPID" 2>/dev/null
kill -TERM "$EPID" 2>/dev/null
sleep 3
echo "# soak 结束: 有效样本 $SCOUNT 行, 运行 $i 秒, 事件 $(ls "$DIR"/ev_*.h264 2>/dev/null | wc -l) 个" >> "$DIR/samples.csv"
echo "soak 完成: $DIR/samples.csv (有效样本 $SCOUNT 行, 运行 $i 秒, 事件 $(ls "$DIR"/ev_*.h264 2>/dev/null | wc -l))"
