#!/bin/sh
# suspend_test.sh — 停流→系统 suspend→resume→再开流 循环
# 用法: sh suspend_test.sh <轮数> [结果目录]
#   默认 20 轮
# ⚠️ 必须 nohup/setsid 后台跑:suspend 期间网络断,ssh 会话会死;日志全写文件。
# 每轮记录:resume 后到首帧成功的耗时(恢复耗时)+ 首帧指标
# ⚠️ 已知约束(L4 实测):带流 suspend 会挂死 → 本脚本严格"先停流再休眠"

N=${1:-20}
DIR=${2:-/root/reg/suspend}
mkdir -p "$DIR"
LOG="$DIR/suspend.log"
: > "$LOG"

# ⚠️ 安全闸门(依据 docs/27 §7):本板**没有 RTC**(`/dev/rtc*` 不存在)、**也没有电源键**
#    (只有硬开关),不存在任何"人能触发"的唤醒源。真实 suspend 之后只能断电,
#    所以必须先确认唤醒源可用并读回校验:
#      能装上 RTC 闹钟 → real(真挂起,120 s 后自动唤醒)
#      装不上         → 自动降级 sim(`pm_test=devices`,5 秒后内核自己恢复,不用断电)
WAKEALARM=/sys/class/rtc/rtc0/wakealarm
SUSP_MODE=sim
if [ -w "$WAKEALARM" ]; then
	_t=$(( $(date +%s) + 120 ))
	echo 0 > "$WAKEALARM" 2>/dev/null
	echo "$_t" > "$WAKEALARM" 2>/dev/null
	if [ "$(cat "$WAKEALARM" 2>/dev/null)" = "$_t" ]; then
		SUSP_MODE=real
	else
		echo 0 > "$WAKEALARM" 2>/dev/null
	fi
fi
if [ "$SUSP_MODE" = "sim" ]; then
	echo devices > /sys/power/pm_test 2>/dev/null
	echo "suspend 模式: sim(**无唤醒源**,改用 pm_test=devices 模拟;耗时不代表真实挂起/恢复)" | tee -a "$LOG"
	echo "suspend 模式: sim" >> "$DIR/suspend.mode"
else
	echo none > /sys/power/pm_test 2>/dev/null
	echo "suspend 模式: real(RTC wakealarm 已就绪,120 s 后自动唤醒)" | tee -a "$LOG"
	echo "suspend 模式: real" >> "$DIR/suspend.mode"
fi

DEV_ISP=/dev/video5
DEV_CIF=/dev/video0

echo "suspend 回归开始: $N 轮(每轮:确认无流→suspend→resume→ISP+CIF 各开一次流)" | tee -a "$LOG"

i=1
fail=0
while [ $i -le $N ]; do
    # 0) 确认没有进程占用视频节点(fuser 无输出=干净)
    busy=$(fuser $DEV_ISP $DEV_CIF 2>/dev/null)
    if [ -n "$busy" ]; then
        echo "轮 $i: [SKIP] 节点被占用(pid=$busy),先清" | tee -a "$LOG"
        for p in $busy; do kill -9 $p 2>/dev/null; done
        sleep 2
    fi

    # 1) suspend(实时钟记录入睡前时刻)
    t_before=$(date +%s.%N)
    echo mem > /sys/power/state 2>>"$LOG"
    rc=$?
    t_after=$(date +%s.%N)
    if [ $rc -ne 0 ]; then
        echo "轮 $i: [FAIL] suspend 写入失败 rc=$rc errno 详见 log" | tee -a "$LOG"
        dmesg | tail -30 > "$DIR/fail_${i}_suspend.txt"
        fail=$((fail + 1))
        i=$((i + 1)); continue
    fi
    # resume 回到这里;suspend 耗时(含睡眠)按实时钟差
    susp_s=$(awk "BEGIN{printf \"%.1f\", $t_after - $t_before}")

    # 2) resume 后:ISP 路开一次流(拿首帧耗时作为"恢复耗时")
    r1=$(./reg_switch $DEV_ISP 3840 2160 NV12 5 2>&1)
    # 3) CIF 路
    r2=$(./reg_switch $DEV_CIF 1632 1224 BG10 5 2>&1)

    ok1=$(echo "$r1" | grep -c "RESULT ok" || true)
    ok2=$(echo "$r2" | grep -c "RESULT ok" || true)
    if [ "$ok1" = "1" ] && [ "$ok2" = "1" ]; then
        first=$(echo "$r1" | sed 's/.*first_ms=\([0-9.]*\).*/\1/')
        echo "轮 $i: OK susp_s=${susp_s} resume_first_ms=$first | $r1 | $r2" >> "$LOG"
        echo "RESULT ok round=$i susp_s=$susp_s resume_first_ms=$first" >> "$LOG"
    else
        echo "轮 $i: [FAIL] resume 后开流失败" | tee -a "$LOG"
        echo "RESULT fail round=$i isp=$r1 cif=$r2" >> "$LOG"
        dmesg | tail -60 > "$DIR/fail_${i}.txt"
        media-ctl -d /dev/media0 -p > "$DIR/fail_${i}_media0.txt" 2>&1
        media-ctl -d /dev/media1 -p > "$DIR/fail_${i}_media1.txt" 2>&1
        fail=$((fail + 1))
        # 恢复尝试:重启 3A daemon(它的状态机可能没跟上)
        systemctl restart rkaiq_3A 2>/dev/null
        sleep 3
    fi
    echo "[进度] $i / $N (fail=$fail)"
    i=$((i + 1))
done
echo "suspend 回归完成: $N 轮,失败 $fail" | tee -a "$LOG"
# 归还 pm_test,避免影响后续真实休眠测试
echo none > /sys/power/pm_test 2>/dev/null
echo "suspend 模式: $SUSP_MODE(结束)" | tee -a "$LOG"
