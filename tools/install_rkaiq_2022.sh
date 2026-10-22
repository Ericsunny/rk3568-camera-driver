#!/bin/bash
# install_rkaiq_2022.sh — 把板上的 rkaiq 换回与 4.19.232 BSP 配套的“2022 三件套”
#
# 背景（详见 docs/21 §9）：
#   YCKJ 镜像里 librkaiq.so / rkaiq_3A_server 是 2024 版（AIQ 6.0x6.1，2024-03-03），
#   但 IQ 是 2022 schema，内核是 4.19.232 → 3A 结果下不到 ISP（Can not get isp params buffer
#   每轮上百次）、AE 完全不收敛、AWB 崩（缺 smartRun）。
#   板子上本来就留着 2022 版的备份（.bak-2022），它们的 md5 与 SDK 里
#   camera_engine_rkaiq_rk3568_arm64.deb（2022-04-08）内的文件逐字节相同。
#
# 用法：
#   bash install_rkaiq_2022.sh            # 装 2022 套件（2024 版自动备份为 .bak-2024）
#   bash install_rkaiq_2022.sh --rollback # 回滚到 2024 版
#   bash install_rkaiq_2022.sh --check    # 只打印当前版本
#
# 注意：换完后要重启一次才干净（rkaiq 被杀后 ISP 可能留在 stream 未停的状态）。
set -u

IQ=/etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json
LIB=/usr/lib/librkaiq.so
SRV=/usr/bin/rkaiq_3A_server
MD5_LIB_2022=654dc84e89edc0aae9ee5f646732ba62
MD5_LIB_2024=be51ce86c4e5fdc9d702ead2311d89a6
MD5_SRV_2022=bc67a0fc6ca1221923132c65c3381aed
MD5_SRV_2024=7640afb5b9c2133a865d4f93f3d9789d
MD5_IQ_2022=0666ba18ce1e4db30cddc92810587fb1

die() { echo "ERROR: $*" >&2; exit 1; }

show() {
    echo "--- 当前 ---"
    md5sum "$LIB" "$SRV" 2>/dev/null
    [ -f "$IQ" ] && md5sum "$IQ"
    [ -f "$IQ.bak-2022" ] && md5sum "$IQ.bak-2022"
    echo "--- 备份 ---"
    ls -l "$LIB".bak-* "$SRV".bak-* 2>/dev/null
}

case "${1:---install}" in
--check)
    show
    exit 0
    ;;
--rollback)
    [ -f "$LIB.bak-2024" ] || die "没有 $LIB.bak-2024，无法回滚"
    [ -f "$SRV.bak-2024" ] || die "没有 $SRV.bak-2024，无法回滚"
    pkill -x rkaiq_3A_server 2>/dev/null
    cp -f "$LIB.bak-2024" "$LIB"
    cp -f "$SRV.bak-2024" "$SRV"
    if [ -f /root/iq.2024.json.bak ]; then cp -f /root/iq.2024.json.bak "$IQ"; fi
    sync
    show
    echo "已回滚到 2024 版，建议 reboot。"
    exit 0
    ;;
--install) ;;
*) die "未知参数 $1（支持 --install/--rollback/--check）" ;;
esac

# ---- 前置检查：三份 2022 备份必须在 ----
[ -f "$LIB.bak-2022" ] || die "缺 $LIB.bak-2022"
[ -f "$SRV.bak-2022" ] || die "缺 $SRV.bak-2022"
[ -f "$IQ.bak" ]       || die "缺 $IQ.bak（2022 版 IQ，331401 B）"

m=$(md5sum "$LIB.bak-2022" | cut -d' ' -f1)
[ "$m" = "$MD5_LIB_2022" ] || die "librkaiq.so.bak-2022 md5=$m，期望 $MD5_LIB_2022"
m=$(md5sum "$SRV.bak-2022" | cut -d' ' -f1)
[ "$m" = "$MD5_SRV_2022" ] || die "rkaiq_3A_server.bak-2022 md5=$m，期望 $MD5_SRV_2022"
m=$(md5sum "$IQ.bak" | cut -d' ' -f1)
[ "$m" = "$MD5_IQ_2022" ] || die "IQ .bak md5=$m，期望 $MD5_IQ_2022"

pkill -x rkaiq_3A_server 2>/dev/null
sleep 1

# ---- 备份 2024 版 + 装 2022 版 ----
cur=$(md5sum "$LIB" | cut -d' ' -f1)
[ "$cur" = "$MD5_LIB_2024" ] && cp -f "$LIB" "$LIB.bak-2024"
cur=$(md5sum "$SRV" | cut -d' ' -f1)
[ "$cur" = "$MD5_SRV_2024" ] && cp -f "$SRV" "$SRV.bak-2024"
if [ "$(md5sum "$IQ" | cut -d' ' -f1)" != "$MD5_IQ_2022" ]; then
    cp -f "$IQ" /root/iq.2024.json.bak
fi

cp -f "$LIB.bak-2022" "$LIB"
cp -f "$SRV.bak-2022" "$SRV"
cp -f "$IQ.bak" "$IQ"
chmod 755 "$SRV"
sync

echo "--- 安装后 ---"
show

# ---- 快速自检 ----
/etc/init.d/rkaiq_3A.sh start >/dev/null 2>&1 || true
sleep 3
if pidof rkaiq_3A_server >/dev/null; then echo "[ok] rkaiq_3A_server 已在跑"; else echo "[warn] rkaiq_3A_server 没起来"; fi
/etc/init.d/rkaiq_3A.sh stop >/dev/null 2>&1 || true
echo "完成。建议 reboot 后手动 /etc/init.d/rkaiq_3A.sh start 使用。"
