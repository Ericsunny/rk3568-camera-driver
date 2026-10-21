#!/bin/bash
# 同场景 A/B:rkaiq AWB/AE vs mini3a 用户态闭环
set -u
AB=/root/ab
mkdir -p $AB
DEV=/dev/video5
SUB=/dev/v4l-subdev6
W=3840
H=2160
FRAMES=180
SCALE=$((W * H))

say() { echo; echo "########## $* ##########"; }

cpu_ticks() { awk '{print $14, $15}' /proc/$1/stat 2>/dev/null || echo "0 0"; }

exact_stats() {   # $1=file
    python3 - "$1" "$W" "$H" <<'PY'
import sys
f = sys.argv[1]; w = int(sys.argv[2]); h = int(sys.argv[3])
d = open(f, 'rb').read()
ys = d[:w*h]
uv = d[w*h:]
y = sum(ys[::8]) / len(ys[::8])
u = sum(uv[0::16]) / len(uv[0::16])
v = sum(uv[1::16]) / len(uv[1::16])
print("  %s: Y=%.2f U=%.2f (%+.2f) V=%.2f (%+.2f)  [每8个像素抽1, UV每8对抽1]" %
      (f.split('/')[-1], y, u, u-128, v, v-128))
PY
}

reset_sensor() {
    v4l2-ctl -d $SUB --set-ctrl exposure=300    >/dev/null 2>&1
    v4l2-ctl -d $SUB --set-ctrl analogue_gain=4 >/dev/null 2>&1
    sleep 0.5
    echo "  起点: $(v4l2-ctl -d $SUB --get-ctrl exposure,analogue_gain 2>/dev/null | tr '\n' ' ')"
}

# ---------------------------------------------------------------- A 臂
say "A 臂:rkaiq_3A_server (librkaiq AIQ, ISP 内核态 3A)"
/etc/init.d/rkaiq_3A.sh stop >/dev/null 2>&1
pkill -f rkaiq_3A_server 2>/dev/null
sleep 1
reset_sensor

/usr/bin/rkaiq_3A_server > $AB/rkaiq.log 2>&1 &
sleep 4
PID=$(pidof rkaiq_3A_server | awk '{print $1}')
echo "  daemon pid=$PID"
T0_U=0; T0_S=0
if [ -n "$PID" ]; then
    set -- $(cpu_ticks $PID); T0_U=$1; T0_S=$2
fi

/root/grab $DEV $FRAMES $AB/A.nv12 $W $H NV12 2>&1 | tee $AB/A_grab.log

T1_U=0; T1_S=0
if [ -n "$PID" ]; then
    set -- $(cpu_ticks $PID); T1_U=$1; T1_S=$2
fi
DT=$(( (T1_U - T0_U) + (T1_S - T0_S) ))
echo "  3A 进程 CPU: $DT ticks = $(echo "scale=3; $DT/100" | bc) s / $FRAMES 帧 = $(echo "scale=2; $DT*10/$FRAMES" | bc) ms/帧"
/etc/init.d/rkaiq_3A.sh stop >/dev/null 2>&1
pkill -f rkaiq_3A_server 2>/dev/null
sleep 1

# ---------------------------------------------------------------- B 臂
say "B 臂:mini3a 用户态闭环(300 帧上限)"
reset_sensor
START=$(date +%s)
timeout 120 /root/mini3a/mini3a -d $DEV -s $SUB -w $W -h $H -n 300 > $AB/B_mini3a.log 2>&1
RC=$?
END=$(date +%s)
echo "  rc=$RC 耗时 $((END-START)) s"
tail -12 $AB/B_mini3a.log

say "结果汇总"
echo "--- A 臂(rkaiq)grab 采样曲线 ---"
grep -E "帧 +[0-9]+:" $AB/A_grab.log | tail -7
echo "--- A 臂末帧精确统计 ---"
exact_stats $AB/A.nv12
echo
echo "--- B 臂(mini3a)状态行 ---"
grep -E "^f=" $AB/B_mini3a.log | tail -8
grep -E "LOCKED at|最终" $AB/B_mini3a.log
echo "--- B 臂末帧精确统计 ---"
[ -f /root/mini3a/final.nv12 ] && exact_stats /root/mini3a/final.nv12
echo
ls -la $AB/A.nv12 /root/mini3a/final.nv12 2>/dev/null
echo OK
