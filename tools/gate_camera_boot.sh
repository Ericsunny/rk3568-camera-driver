#!/bin/sh
# gate_camera_boot.sh —— 摄像头在位启动回归门禁(板端执行)
#
# 用途:任何改动 get_fmt/set_fmt 或 rkcif 协商路径的提交,上板后必须先过这一关。
# 用法:
#   1) 摄像头排线插好;2) 断电 ≥10s 后上电(必须冷启动,dmesg 才是本次启动的);
#   3) 登录后执行: sh gate_camera_boot.sh ["#52"]
#      第二个参数可选,给出则校验 uname 里包含该串(如 "#52")。
# 退出码: 0=PASS, 1=FAIL(逐条打印失败原因)
#
# 判据(缺一不可):
#   a) uname -a 含期望内核标识(若给出)
#   b) dmesg 出现 "Detected OV8858 id 0x8858"(sensor 真的 probe 成功)
#   c) dmesg 无 "sensor fmt invalid"
#   d) dmesg 无 "rkcif_set_fmt"
#   e) dmesg 无 "Kernel panic"
#   f) media-ctl -p -d /dev/media0 的 ENABLED 链路数 ≥ 12
#
# 注意:dmesg 只保留本次启动的日志,所以必须冷启动后立刻跑;
#       若板子刚从某个 wedge/D 状态恢复,先重启再跑。

EXPECT="$1"
FAIL=0
say() { printf '%s\n' "$*"; }

say "== 摄像头在位启动门禁 =="

U="$(uname -a)"
say "uname            : $U"
if [ -n "$EXPECT" ]; then
	if printf '%s' "$U" | grep -q -- "$EXPECT"; then
		say "  [OK] 含期望标识 $EXPECT"
	else
		say "  [FAIL] 不含期望标识 $EXPECT"
		FAIL=1
	fi
fi

C="$(dmesg | grep -c 'Detected OV8858 id 0x8858')"
say "sensor probe     : $C 次"
[ "$C" -ge 1 ] || { say "  [FAIL] sensor 未 probe(检查排线/供电)"; FAIL=1; }

for pat in "sensor fmt invalid" "rkcif_set_fmt" "Kernel panic"; do
	N="$(dmesg | grep -c -- "$pat")"
	say "dmesg '$pat' : $N"
	[ "$N" -eq 0 ] || { say "  [FAIL] 命中 '$pat'"; FAIL=1; }
done

E="$(media-ctl -p -d /dev/media0 2>/dev/null | grep -c '\[ENABLED\]')"
say "ENABLED links    : $E"
[ "$E" -ge 12 ] || { say "  [FAIL] ENABLED 链路 < 12(拓扑不完整)"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
	say "GATE: PASS"
else
	say "GATE: FAIL —— 不要合入/继续改 get_fmt/set_fmt,先取 dmesg 与 media-ctl -p 现场"
fi
exit "$FAIL"
