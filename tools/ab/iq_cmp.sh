#!/bin/bash
# 对比:原始 IQ vs 打了 smartRun 的 IQ,看 rkaiq 是否报 "invalid main scene len!"
IQ=/etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json
PATCHED=/root/iq.patched.json
cp -f $IQ $PATCHED 2>/dev/null

try_one() {
    local tag=$1
    echo "===== $tag ====="
    /etc/init.d/rkaiq_3A.sh stop >/dev/null 2>&1
    pkill -f rkaiq_3A_server 2>/dev/null
    sleep 1
    rm -f /tmp/rk.log
    /usr/bin/rkaiq_3A_server > /tmp/rk.log 2>&1 &
    sleep 6
    PID=$(pidof rkaiq_3A_server | awk '{print $1}')
    echo "  daemon alive: ${PID:-NO(已崩)}"
    echo "  iq 文件行: $(grep -c 'iq:' /tmp/rk.log)"
    grep -E "iq:|invalid main scene|parse sensor|get isp or ispp|Segmentation|ERROR|E:[A-Za-z]" /tmp/rk.log | head -8 | sed 's/^/    /'
    pkill -f rkaiq_3A_server 2>/dev/null
    sleep 1
}

cp -f /root/iq.orig.json $IQ
try_one "原始 IQ(788267 B,无 smartRun)"

cp -f $PATCHED $IQ
try_one "打过 smartRun 的 IQ(788677 B)"

ls -la $IQ
echo OK
