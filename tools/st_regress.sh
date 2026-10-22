#!/bin/bash
# st_regress.sh —— 板端真实回归:开流循环 / 连续采集 / 停流后休眠恢复 / 故障取证
# 用法: sh /root/st_regress.sh [--cycles N] [--long S] [--with-suspend]
# 产物: /root/st_report.txt(一条命令生成的报告) + /root/st_logs/<ts>/ 取证

DEV=/dev/video5
W=1920
H=1080
CYCLES=200
LONGSEC=120
SUSP=0
while [ $# -gt 0 ]; do
	case "$1" in
	--cycles) shift; CYCLES="$1" ;;
	--long)   shift; LONGSEC="$1" ;;
	--with-suspend) SUSP=1 ;;
	esac
	shift
done

TS=$(date +%m%d_%H%M%S)
LOG=/root/st_logs/$TS
REP=/root/st_report.txt
mkdir -p "$LOG"
: > "$REP"
say() { echo "$@" | tee -a "$REP"; }
log() { echo "$@" >> "$LOG/run.log"; }

# ---------- 故障取证 ----------
dump_fault() {
	local tag="$1"
	mkdir -p "$LOG/$tag"
	dmesg | tail -80 > "$LOG/$tag/dmesg.txt"
	media-ctl -p -d /dev/media0 > "$LOG/$tag/media0.txt" 2>&1
	media-ctl -p -d /dev/media1 > "$LOG/$tag/media1.txt" 2>&1
	ls -l /dev/video* /dev/v4l-subdev* > "$LOG/$tag/nodes.txt" 2>&1
	echo "fault=$tag @ $(date +%H:%M:%S)" >> "$LOG/run.log"
}

# ---------- 复位尝试 ----------
try_recover() {
	local t0 t1
	t0=$(date +%s%N)
	if [ "$(pgrep -f '[g]rab' | wc -l)" != "0" ]; then
		pkill -f '[g]rab'; sleep 2
	fi
	systemctl stop rkaiq_3A >/dev/null 2>&1
	sleep 2
	/root/st_burn -d "$DEV" -w "$W" -h "$H" -c 3 >/dev/null 2>&1
	local rc=$?
	t1=$(date +%s%N)
	echo $(( (t1 - t0) / 1000000 ))
	return $rc
}

say "# 板端出流稳定性回归报告"
say ""
say "生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
say "内核: $(uname -r) $(uname -v)"
say "设备: $DEV ${W}x${H} NV12, 循环 $CYCLES 次, 连续采集 ${LONGSEC}s, 休眠恢复: $([ $SUSP = 1 ] && echo 测 || echo 未测)"
say "取证目录: $LOG"
say ""

# ---------- 预检 ----------
say "## 0. 预检"
say "- rkaiq_3A: is-enabled=$(systemctl is-enabled rkaiq_3A 2>&1) is-active=$(systemctl is-active rkaiq_3A 2>&1) procs=$(pgrep -f '[r]kaiq_3A_server' | wc -l)"
say "- 处置: 停 3A(压测必须独占 ISP)"
systemctl stop rkaiq_3A >/dev/null 2>&1
sleep 1
say "- 停后 procs=$(pgrep -f '[r]kaiq_3A_server' | wc -l)"
say ""

VERDICT=PASS

# ---------- P1 开关流 ----------
say "## 1. 反复开关流（每轮 open→S_FMT→REQBUFS→STREAMON→首帧→STREAMOFF→close）"
C1=$(/root/st_burn -d "$DEV" -w "$W" -h "$H" -c "$CYCLES" 2>"$LOG/cycles.err")
RC1=$?
echo "$C1" >> "$LOG/run.log"
say "- $C1"
F1=$(echo "$C1" | grep -c 'fail=0 fail_pct=0.00')
if [ "$RC1" != "0" ]; then
	dump_fault p1_cycles
	RT=$(try_recover); RRC=$?
	say "- ❌ 有失败轮次(rc=$RC1), 已取证; 自动恢复尝试 rc=$RRC 耗时 ${RT} ms"
	VERDICT=FAIL
fi
say ""

# ---------- P2 连续采集 ----------
say "## 2. 长时间连续采集"
C2=$(/root/st_burn -d "$DEV" -w "$W" -h "$H" -t "$LONGSEC")
RC2=$?
echo "$C2" >> "$LOG/run.log"
say "- $C2"
if [ "$RC2" != "0" ]; then
	dump_fault p2_long
	say "- ❌ 采集异常(rc=$RC2), 已取证"
	VERDICT=FAIL
fi
say ""

# ---------- P3 休眠恢复 ----------
# ⚠️ 安全闸门:本板没有 RTC(/dev/rtc* 不存在)、也没有电源键(只有硬开关),
#    没有任何"人能触发"的唤醒源。真实 suspend 之后只能断电,所以这里必须先确认
#    唤醒源可用并读回校验,不通过就跳过,绝不让板子睡死。详见 docs/27 §7。
if [ $SUSP = 1 ]; then
	say "## 3. 停流后休眠 / 唤醒 / 恢复出流"
	sleep 2     # 等 ISP 空闲,runtime PM 自行挂起
	WAKEALARM=/sys/class/rtc/rtc0/wakealarm
	WAKE_OK=0
	if [ -w "$WAKEALARM" ]; then
		echo 0 > "$WAKEALARM" 2>/dev/null
		TARGET=$(( $(date +%s) + 10 ))
		echo "$TARGET" > "$WAKEALARM" 2>/dev/null
		if [ "$(cat "$WAKEALARM" 2>/dev/null)" = "$TARGET" ]; then
			WAKE_OK=1
			say "- 唤醒源已就绪: RTC wakealarm @ $TARGET"
		fi
	fi
	if [ $WAKE_OK != 1 ]; then
		say "- **跳过 suspend**:本板无可用唤醒源(无 RTC、无电源键),真实 suspend 后只能断电"
		say "- 依据: docs/27 §7;如需验证,请先接串口或补 PMIC RTC 设备节点"
		echo "RESULT=SKIPPED_NO_WAKEUP_SOURCE" >> "$LOG/suspend.err"
	else
		T0=$(date +%s%N)
		echo mem > /sys/power/state 2>>"$LOG/suspend.err"
		T1=$(date +%s%N)
		sleep 1
		RESUME_MS=$(( (T1 - T0) / 1000000 ))
		say "- 进入/退出 suspend 耗时: ${RESUME_MS} ms"
		say "- 唤醒后内核: $(uname -v)"
	fi
	R0=$(date +%s%N)
	C3=$(/root/st_burn -d "$DEV" -w "$W" -h "$H" -c 20)
	RC3=$?
	R1=$(date +%s%N)
	say "- 恢复出流(20 轮): $C3"
	say "- 恢复耗时: $(( (R1 - R0) / 1000000 )) ms, rc=$RC3"
	if [ "$RC3" != "0" ]; then
		dump_fault p3_resume
		say "- ❌ 唤醒后无法恢复出流"
		VERDICT=FAIL
	fi
	say ""
fi

# ---------- 汇总 ----------
say "## 4. 结论"
say "- 失败率: $(echo "$C1" | sed -n 's/.*fail_pct=\([0-9.]*\).*/\1/p')%"
say "- 掉帧率: $(echo "$C2" | sed -n 's/.*loss_pct=\([0-9.]*\).*/\1/p')%"
say "- 总判定: **$VERDICT**"
say ""
say "## 5. 原始行"
say '```'
say "P1: $C1"
say "P2: $C2"
say '```'

# 恢复 3A 到开机状态
systemctl start rkaiq_3A >/dev/null 2>&1
echo "报告: $REP"
echo "取证: $LOG"
exit 0
