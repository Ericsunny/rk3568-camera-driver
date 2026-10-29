#!/bin/sh
# v7mon.sh — V7 soak 资源采样器: 用法 v7mon.sh <evrec_pid> <输出文件>
# 每 60s 一针: rss kB / fd 数 / 线程数; 目标进程消失即退出(补一行标记)
PID=$1
OUT=$2
[ -n "$PID" ] && [ -n "$OUT" ] || { echo "usage: $0 <pid> <outfile>" >&2; exit 2; }
while kill -0 "$PID" 2>/dev/null; do
	rss=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null)
	fds=$(ls "/proc/$PID/fd" 2>/dev/null | wc -l)
	thr=$(awk '/Threads/ {print $2}' "/proc/$PID/status" 2>/dev/null)
	echo "$(date +%H:%M:%S) rss=${rss}kB fds=$fds threads=$thr"
	sleep 60
done
echo "$(date +%H:%M:%S) server gone, monitor exit"
