#!/bin/sh
# camera_selfcheck.sh —— 这台板子"相机能不能用"的一键自检
#
# 用法（板上）： sh camera_selfcheck.sh [-q]
#   -q  跳过实拍（只查配置，约 1 秒）
#
# 判定：退出码 0 = 全部通过；非 0 = 有项失败（失败项以 [FAIL] 标出）
#
# 检查项（每一项都对应过真实踩过的坑）：
#   1. 内核里有没有我们补的驱动符号（内建驱动 =y，靠 /proc/kallsyms 判断）
#   2. media 实体名是否符合 Rockchip 约定（m00_b_imx415 / ov8858-sm）
#   3. 关键设备节点是否都在
#   4. rkaiq 三件套 + IQ 是不是配套的 2022 版（md5）
#   5. 3A daemon 是否在跑、是否只有一个、日志里有没有 params buffer 报错
#   6. 实拍 60 帧：能不能出流、帧率、亮度是否落在合理区间

QUICK=0
[ "$1" = "-q" ] || [ "$1" = "--quick" ] && QUICK=1

FAIL=0
ok()   { echo "[ OK ] $*"; }
bad()  { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
info() { echo "       $*"; }

IQ=/etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json

echo "===== 1. 内核驱动 ====="
if grep -q ov8858_sm_enum_frame_size /proc/kallsyms 2>/dev/null; then
    ok "补丁驱动已生效（ov8858_sm_enum_frame_size 在 kallsyms 里）"
else
    bad "内核里没有 ov8858_sm_enum_frame_size —— 刷的还是旧 Image"
    info "重编+换 /boot/Image-4.19.232 的流程见 docs/17 §10"
fi
info "kernel: $(uname -r) / build: $(uname -v)"

echo "===== 2. media 实体 ====="
TOPO=$(media-ctl -d /dev/media1 --print-topology 2>/dev/null)
if echo "$TOPO" | grep -q "m00_b_imx415"; then
    ok "rkisp 侧实体名合规（m00_b_imx415）"
else
    bad "rkisp 侧找不到 m00_b_imx415 —— 3A 会认不出 sensor"
fi
TOPO0=$(media-ctl -d /dev/media0 --print-topology 2>/dev/null)
# M4 起驱动把实体名改成 Rockchip 约定 "m<NN>_<b|f>_<sensor> <bus>-<addr>"，
# 老版本是 v4l2_i2c_subdev_init 默认的 "ov8858-sm 5-0036"，这里两种都认。
if echo "$TOPO0" | grep -Eq 'm[0-9]{2}_[bf]_ov8858'; then
    ok "rkcif 侧 OV8858 实体名合规（$(echo "$TOPO0" | grep -Eo 'm[0-9]{2}_[bf]_ov8858[^"]*' | head -1)）"
elif echo "$TOPO0" | grep -q "ov8858-sm"; then
    ok "rkcif 侧 OV8858 实体在，但名字还是旧的（ov8858-sm …）；2022 库容忍，2024 库会拒绝"
else
    bad "rkcif 侧找不到 ov8858 实体"
fi

echo "===== 3. 设备节点 ====="
for d in /dev/video5 /dev/video12 /dev/video13 /dev/v4l-subdev3 /dev/v4l-subdev6 /dev/media0 /dev/media1; do
    if [ -e "$d" ]; then ok "$d"; else bad "$d 不存在"; fi
done

echo "===== 4. 3A 三件套与 IQ ====="
check_md5() {  # 文件 期望md5 说明
    got=$(md5sum "$1" 2>/dev/null | cut -d' ' -f1)
    if [ "$got" = "$2" ]; then ok "$3 md5 正确（$got）"; else bad "$3 md5=$got，期望 $2"; fi
}
check_md5 /usr/lib/librkaiq.so     654dc84e89edc0aae9ee5f646732ba62 "librkaiq.so（2022）"
check_md5 /usr/bin/rkaiq_3A_server bc67a0fc6ca1221923132c65c3381aed "rkaiq_3A_server（2022）"

if [ -f "$IQ" ]; then
    if python3 -c "import json;json.load(open('$IQ'))" 2>/dev/null; then
        ok "IQ JSON 合法（$(stat -c%s "$IQ") 字节）"
        OE=$(grep -A2 '"OverExpCtrl"' "$IQ" | grep '"Enable"' | head -1 | tr -dc '0-9')
        info "高光优先(OverExpCtrl)=${OE:-?}  切换: python3 iq_highlight_mode.py on|off"
    else
        bad "IQ JSON 解析失败：$IQ"
    fi
else
    bad "IQ 文件不存在：$IQ"
fi

echo "===== 5. 3A daemon ====="
if systemctl is-active rkaiq_3A >/dev/null 2>&1; then
    ok "systemd 单元 active"
else
    bad "rkaiq_3A 未运行（systemctl start rkaiq_3A）"
fi
N=$(ps -ef | grep rkaiq_3A_server | grep -v grep | wc -l)
if [ "$N" -eq 1 ]; then
    ok "只有 1 个 daemon 进程"
elif [ "$N" -eq 0 ]; then
    bad "没有 daemon 进程"
else
    bad "有 $N 个 daemon 进程 —— 会互相抢 params 设备，必须只留一个"
fi
if journalctl -u rkaiq_3A -n 200 --no-pager 2>/dev/null | grep -q "Can not get isp params buffer"; then
    bad "日志里有 'Can not get isp params buffer' —— 3A 参数下发失败"
else
    ok "日志里没有 params buffer 报错"
fi

echo "===== 6. 实拍 ====="
if [ "$QUICK" = "1" ]; then
    info "跳过（-q）"
else
    OUT=/tmp/selfcheck.nv12
    LOG=/tmp/selfcheck.log
    timeout 30 /root/grab /dev/video5 60 $OUT > $LOG 2>&1
    LINE=$(grep '帧' $LOG | grep 'Y ' | tail -1)
    if [ -z "$LINE" ]; then
        bad "60 帧没采到（设备被占或已卡死，通常需要重启）"
    else
        Y=$(echo "$LINE" | sed -n 's/.*Y *\([0-9][0-9.]*\).*/\1/p')
        FPS=$(sed -n 's/.*, *\([0-9][0-9.]*\) fps).*/\1/p' $LOG | tail -1)
        ok "出流正常：$LINE"
        if [ -n "$FPS" ]; then
            awk -v f="$FPS" 'BEGIN{ if (f+0 >= 10) exit 0; exit 1 }' \
                && ok "帧率 ${FPS} fps（≥10）" || bad "帧率 ${FPS} fps 偏低"
        fi
        if [ -n "$Y" ]; then
            awk -v y="$Y" 'BEGIN{ if (y+0 > 8 && y+0 < 200) exit 0; exit 1 }' \
                && ok "亮度 Y=${Y} 落在合理区间（8~200）" \
                || bad "亮度 Y=${Y} 异常（过暗/过曝，查曝光或镜头盖）"
        fi
    fi
fi

echo "===== 7. 双摄并行（L3）====="
if [ "$QUICK" = "1" ]; then
    info "跳过（-q）"
else
    for d in /dev/video0; do
        [ -e "$d" ] && ok "$d 在" || bad "$d 不在（OV8858/rkcif 路）"
    done
    # 两路同时在跑：rkisp 走 IMX415，rkcif 走 OV8858，验证互不干扰
    timeout 45 /root/grab /dev/video0 60 /tmp/dual_ov.nv12 1632 1224 > /tmp/dual_ov.log 2>&1 &
    P_OV=$!
    timeout 45 /root/grab /dev/video5 60 /tmp/dual_ic.nv12 > /tmp/dual_ic.log 2>&1
    wait $P_OV
    FPS_OV=$(sed -n 's/.*, *\([0-9][0-9.]*\) fps).*/\1/p' /tmp/dual_ov.log | tail -1)
    FPS_IC=$(sed -n 's/.*, *\([0-9][0-9.]*\) fps).*/\1/p' /tmp/dual_ic.log | tail -1)
    if [ -n "$FPS_OV" ] && [ -n "$FPS_IC" ]; then
        ok "双路同时出流：OV8858 ${FPS_OV} fps + IMX415 ${FPS_IC} fps"
        SUM=$(awk -v a="$FPS_OV" -v b="$FPS_IC" 'BEGIN{printf "%.1f", a+b}')
        BW=$(awk -v a="$FPS_OV" -v b="$FPS_IC" \
            'BEGIN{printf "%.0f", a*2996352/1048576 + b*12441600/1048576}')
        info "合计 ${SUM} fps / 约 ${BW} MB/s 原始码流"
    else
        bad "双摄并行失败（OV8858 fps='${FPS_OV}' IMX415 fps='${FPS_IC}'）"
        info "看 /tmp/dual_ov.log 与 /tmp/dual_ic.log；若 rkcif 侧 0 帧，多半是设备被占"
    fi
fi

echo "====="
if [ "$FAIL" -eq 0 ]; then
    echo "结论：全部通过 ✅"
    exit 0
else
    echo "结论：$FAIL 项失败 ❌"
    exit 1
fi
