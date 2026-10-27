#!/bin/sh
# check_3a_lifecycle.sh —— 3A × 采集 生命周期检查(规则见 docs/29 §10)
#
# 用法:
#   sh check_3a_lifecycle.sh                      # 安全模式(默认):只看状态 + 验 A1
#   sh check_3a_lifecycle.sh --wedge --yes        # 红线模式:验 A2(会让 ISP wedge,必须重启)
#
# 安全模式做三件事(都不破坏现场):
#   1. 打印 3A 服务状态(enabled/active);
#   2. 若 3A active → 采 30 帧,验证 A1(3A 常驻可开流);
#   3. 若 3A inactive → 只告警:若本 boot 3A 启动过,则"停 3A 后直接采"必 wedge(A4′),
#      要无 3A 采集请 disable 后重启(规则 2)。
#
# 红线模式(--wedge)按 A2 执行:后台长流 → stop 3A → 观察帧是否立即停
#   → 终止流触发 STREAMOFF → 预期 rkisp_stream_stop timeout。
#   **执行后 video5 不可用,必须断电重启。**
#
# 退出码:0=符合预期;1=不符合;2=用法错误。

DEV=${DEV:-/dev/video5}
FRAMES=${FRAMES:-30}
MODE=safe
CONFIRM=0

for a in "$@"; do
	case "$a" in
	--wedge) MODE=wedge ;;
	--yes)   CONFIRM=1 ;;
	*) echo "用法: $0 [--wedge --yes]"; exit 2 ;;
	esac
done

say() { printf '%s\n' "$*"; }
EN=$(systemctl is-enabled rkaiq_3A 2>/dev/null)
AC=$(systemctl is-active  rkaiq_3A 2>/dev/null)
say "3A 服务: enabled=$EN active=$AC"

capture() { # capture <帧数> <输出文件>
	timeout 40 v4l2-ctl -d "$DEV" --stream-mmap --stream-count="$1" > "$2" 2>&1
	echo $?
}

if [ "$MODE" = safe ]; then
	if [ "$AC" = active ]; then
		RC=$(capture "$FRAMES" /tmp/_lc_safe.out)
		N=$(tr -cd '<' < /tmp/_lc_safe.out | wc -c)
		say "A1(3A 常驻采集): RC=$RC 帧=$N"
		[ "$RC" = 0 ] && [ "$N" -ge "$FRAMES" ] || { say "A1 FAIL"; exit 1; }
		say "A1 PASS"
	else
		say "3A 未运行。注意:若本 boot 内 3A 启动过,则现在直接采会 params 超时 wedge(A4′);"
		say "      要无 3A 采集,请 'systemctl disable rkaiq_3A' 后重启(A4/E2),再跑本脚本。"
	fi
	exit 0
fi

# ---- 红线模式: A2 ----
if [ "$CONFIRM" != 1 ]; then
	say "红线模式会让 ISP wedge 且只能重启恢复。确认请加 --yes。"
	exit 2
fi
[ "$AC" = active ] || { say "A2 需要 3A active 才能测(当前 $AC)"; exit 1; }

say "启动后台长流 ..."
setsid v4l2-ctl -d "$DEV" --stream-mmap --stream-count=1000000 > /tmp/_lc_a2.out 2>&1 &
P=$!
sleep 3
T1=$(awk '{print $14+$15}' /proc/$P/stat 2>/dev/null)
systemctl stop rkaiq_3A
sleep 4
T2=$(awk '{print $14+$15}' /proc/$P/stat 2>/dev/null)
say "停 3A 后 CPU ticks: $T1 -> $T2(零增长=帧已停,符合 A2)"
kill "$P" 2>/dev/null
sleep 3
TIMEOUT=$(dmesg | grep -c 'rkisp_stream_stop id:0 timeout')
say "STREAMOFF 超时计数: $TIMEOUT"
if [ "$T2" = "$T1" ] && [ "$TIMEOUT" -ge 1 ]; then
	say "A2 CONFIRMED(符合预期:帧立即停 + STREAMOFF 超时)。请断电重启恢复。"
	exit 0
fi
say "A2 与预期不符 —— 保存 dmesg 现场后再重启。"
exit 1
