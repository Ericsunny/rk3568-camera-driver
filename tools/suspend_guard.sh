#!/bin/sh
# suspend_guard.sh —— 应用层护栏: 有任何进程还占着 /dev/video* 时拒绝进入 suspend
#
# 背景(见 docs/27 §7): 本板无 RTC、无远程唤醒源, 真实 suspend 只能断电;
# 带流挂起则连正在跑的采集一起陪葬(内核侧 ov8858_sm 已加 -EBUSY 拒绝,
# 此脚本挡的是"别的节点/别的传感器"与"习惯性 echo mem"的手滑)。
#
# 用法:
#   sh suspend_guard.sh check          # 只检查, 0=无占用可挂, 1=有占用(列出)
#   sh suspend_guard.sh mem            # 检查通过才执行 echo mem > state
#   sh suspend_guard.sh devices        # 同上, 用 pm_test=devices 干跑(安全,
#                                      # 设备 suspend 后立即自动恢复)
# 注意: devices/mem 都会先把 pm_test 设好, 挂起尝试后恢复 pm_test 为 none。

ACT=${1:-check}

USERS=$(ls -l /proc/[0-9]*/fd/* 2>/dev/null |
	grep -o '/dev/video[0-9]*' | sort | uniq)

if [ -n "$USERS" ]; then
	echo "suspend_guard: 拒绝 —— 以下设备仍被占用:"
	for d in $USERS; do
		echo "  $d: $(ls -l /proc/[0-9]*/fd/* 2>/dev/null |
			grep "$d" | sed 's|.*/proc/\([0-9]*\)/fd.*|\1|' |
			sort -un | tr '\n' ' ')"
	done
	echo "先停流(或 kill 上述进程), 再挂起。"
	exit 1
fi

case "$ACT" in
check)
	echo "suspend_guard: 无采集占用, 可以挂起"
	;;
devices)
	echo devices > /sys/power/pm_test
	echo mem > /sys/power/state
	echo none > /sys/power/pm_test
	echo "pm_test=devices 干跑已返回 (rc=$?)"
	;;
mem)
	echo mem > /sys/power/state
	;;
*)
	echo "用法: $0 [check|devices|mem]" >&2
	exit 2
	;;
esac
