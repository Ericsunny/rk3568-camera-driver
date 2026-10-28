#!/bin/sh
# check_claims.sh —— 提交声称核验(防"声称 vs 实际"不符, docs/38 H1 两次教训)
#
# 用法: tools/check_claims.sh <commit> <claims-file>
# claims-file 每行一条: <repo相对路径>:<行号>:<该行必须包含的固定子串>
#   - 以 # 开头的行跳过
#   - 子串按字面(Fixed string)匹配, 不做正则
# 退出码: 全部命中=0; 任一不符=1(并打印不符项)
#
# 例: tools/check_claims.sh HEAD claims.txt
#   claims.txt:
#     tools/rtsp_hub.c:138:RTSP_CLI_MAX 10
#     tools/rtsp_hub.c:1009:453 Too Many Connections
set -u

[ $# -eq 2 ] || { echo "用法: $0 <commit> <claims-file>" >&2; exit 2; }
commit=$1
claims=$2
[ -f "$claims" ] || { echo "claims 文件不存在: $claims" >&2; exit 2; }

pass=0
fail=0
while IFS= read -r line; do
	case "$line" in
		''|'#'*) continue ;;
	esac
	file=${line%%:*}
	rest=${line#*:}
	lineno=${rest%%:*}
	expect=${rest#*:}
	got=$(git show "$commit:$file" 2>/dev/null | sed -n "${lineno}p")
	if [ -z "$got" ]; then
		echo "FAIL $file:$lineno (行不存在或文件不存在)"
		fail=$((fail + 1))
		continue
	fi
	case "$got" in
		*"$expect"*)
			echo "ok   $file:$lineno"
			pass=$((pass + 1))
			;;
		*)
			echo "FAIL $file:$lineno"
			echo "     期望含: $expect"
			echo "     实际行: $got"
			fail=$((fail + 1))
			;;
	esac
done < "$claims"

echo "----"
echo "claims: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
