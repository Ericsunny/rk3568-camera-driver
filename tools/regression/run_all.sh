#!/bin/sh
# run_all.sh — 回归总入口:依次 跑 switch → soak → suspend(后台,网络会断)
# 用法: sh run_all.sh [switch轮数] [soak秒数] [suspend轮数]
#   默认 500 / 7200 / 20
# 说明:前两项 ssh 在线可跑;suspend 期间网络断,自动 setsid 转/守护,
#       全部完成后生成 report.md(见 gen_report.py)。

SW=${1:-500}
SO=${2:-7200}
SU=${3:-20}
ROOT=/root/reg
TS=$(date +%Y%m%d_%H%M%S)
BASE="$ROOT/run_$TS"
mkdir -p "$BASE"
cd "$(dirname "$0")"

echo "===== 回归总入口 ====="
echo "基目录: $BASE"
echo "配置: switch=$SW soak=${SO}s suspend=$SU"

echo "--- [1/3] switch ($SW 轮,交替 ISP/CIF) ---"
sh switch_test.sh "$SW" "$BASE/switch"
RC1=$?

echo "--- [2/3] soak (${SO}s, ISP NV12) ---"
sh soak_test.sh "$SO" "$BASE/soak"
RC2=$?

echo "--- [3/3] suspend ($SU 轮,ssh 将断开,自动后台) ---"
setsid nohup sh -c "
    sh suspend_test.sh $SU $BASE/suspend > $BASE/suspend/stdout.log 2>&1
    cd $BASE && python3 gen_report.py $BASE > $BASE/report.md 2>&1
    touch $BASE/ALL_DONE
" > /dev/null 2>&1 &
echo "suspend 已转后台(pid=$!),ssh 可能断开;完成后 $BASE/ALL_DONE 出现,report.md 就绪"
echo "查看进度(重连后): ls $BASE/ ; tail $BASE/suspend/suspend.log"

[ $RC1 -ne 0 ] && echo "⚠ switch 退出码 $RC1(2=致命停止)"
[ $RC2 -ne 0 ] && echo "⚠ soak 退出码 $RC2"
echo "===== 前两项完成,suspend 后台运行中 ====="
