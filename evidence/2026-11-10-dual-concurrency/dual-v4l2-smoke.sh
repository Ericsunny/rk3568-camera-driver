#!/bin/bash
set -eu
umask 077
b=/root/camera-integration-20261110-build
out=$b/dual-v4l2-smoke-20261110
mkdir "$out"
printf '%s\n' 'Concurrent independent V4L2 streams: IMX415 video5 1920x1080 NV12 and OV8858 video14 1280x960 NV12, 180 frames each to /dev/null, one live 3A. Separate logs/exit status, sample both ISP while concurrent. No guaranteed fps.' > "$out/criteria.txt"
! fuser /dev/video5 /dev/video14 >/dev/null 2>&1
systemctl is-active --quiet rkaiq_3A.service
[ "$(pgrep -fc '^/usr/bin/rkaiq_3A_server([[:space:]]|$)')" -eq 1 ]
date -Is > "$out/start.txt"
uname -a > "$out/uname.txt"
sha256sum /boot/Image-4.19.232 /etc/iqfiles/ov8858_DC-21AF-58-1474V1_unknown.json > "$out/inputs.sha256"
dmesg > "$out/dmesg-before.txt"
for sensor in imx415 ov8858; do
  (
    date -Is > "$out/$sensor-start.txt"
    if [ "$sensor" = imx415 ]; then dev=/dev/video5; fmt=width=1920,height=1080,pixelformat=NV12; else dev=/dev/video14; fmt=width=1280,height=960,pixelformat=NV12; fi
    set +e
    timeout -s TERM -k 4 35 v4l2-ctl -d "$dev" --set-fmt-video="$fmt" --stream-mmap=4 --stream-count=180 --stream-to=/dev/null > "$out/$sensor.log" 2>&1
    rc=$?
    set -e
    echo "$rc" > "$out/$sensor.rc"
    date -Is > "$out/$sensor-end.txt"
    exit "$rc"
  ) &
  if [ "$sensor" = imx415 ]; then a=$!; else c=$!; fi
done
echo "$a $c" > "$out/children.pid"
sleep 3
cat /proc/rkisp-vir0 > "$out/isp0.txt"
cat /proc/rkisp-vir1 > "$out/isp1.txt"
ps -eo pid,ppid,stat,args | grep -E '[v]4l2-ctl|[r]kaiq_3A_server' > "$out/processes.txt" || :
set +e
wait "$a"; arc=$?
wait "$c"; crc=$?
set -e
printf '%s %s\n' "$arc" "$crc" > "$out/status.txt"
dmesg > "$out/dmesg-after.txt"
cnt=$(wc -l < "$out/dmesg-before.txt")
tail -n +$((cnt+1)) "$out/dmesg-after.txt" > "$out/dmesg-delta.txt"
systemctl is-active rkaiq_3A.service > "$out/3a-status.txt" || :
date -Is > "$out/end.txt"
test "$arc" -eq 0 && test "$crc" -eq 0
