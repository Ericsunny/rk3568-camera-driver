#!/bin/sh
# quickstart.sh —— 5 分钟复现入口(板端执行,编排既有件,不引入新逻辑)
# 用法: sh quickstart.sh [期望内核标识,如 "#52"]
# 步骤: 资产核对 → 启动门禁 → 3A 生命周期(安全模式) → 4K 采集冒烟 → 汇总
# 退出码: 0=全过; 1=任一失败;设备被占时 3/4 步跳过但不计失败
T=$(dirname "$0")
EXPECT="${1:-#52}"
FAIL=0
BUSY=0

# 统一占用检测:video5 被任何采流进程持有时,采集类步骤只跳过不判失败
if ls -l /proc/[0-9]*/fd 2>/dev/null | grep -q video5; then
	BUSY=1
	echo "注意: /dev/video5 正被采流进程占用(soak/录制中?)——第 3/4 步将跳过"
fi

step() { printf '\n=== %s ===\n' "$1"; }

step "1/4 板端资产核对(verify_manifest)"
sh "$T/verify_manifest.sh" || FAIL=1

step "2/4 摄像头在位启动门禁(gate_camera_boot)"
sh "$T/gate_camera_boot.sh" "$EXPECT" || FAIL=1

step "3/4 3A 生命周期·安全模式(check_3a_lifecycle)"
if [ "$BUSY" = 1 ]; then
	echo "SKIP: 设备被占(见开头提示)"
else
	sh "$T/check_3a_lifecycle.sh" || FAIL=1
fi

step "4/4 4K 采集冒烟(30 帧)"
if [ "$BUSY" = 1 ]; then
	echo "SKIP: 设备被占"
else
	if timeout 30 v4l2-ctl -d /dev/video5 --stream-mmap --stream-count=30 >/tmp/qs.out 2>&1; then
		N=$(tr -cd '<' </tmp/qs.out | wc -c)
		echo "采集 30 帧请求, 实得 $N 帧"
		[ "$N" -ge 30 ] || FAIL=1
	else
		echo "采集失败(rc=$?)"; FAIL=1
	fi
fi

printf '\n========= quickstart %s =========\n' "$([ $FAIL = 0 ] && echo PASS || echo FAIL)"
echo "后续: 完整复现见 docs/35 §6(7 步);服务栈见 tools/run_3a_stack.sh"
exit $FAIL
