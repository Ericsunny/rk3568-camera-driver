#!/bin/bash
set -eu
umask 077
b=/root/camera-integration-20261110-build/dual-encode-20261110
app=/root/camera-integration-20261110-dual-app
mkdir "$b"
printf '%s\n' 'manual; IMX415 1920x1080:8554; OV8858 1280x960:8555; simultaneous RTSP/TCP decode to null for 10s and manual event; clean shutdown; see local dual-encode-plan.md' > "$b/criteria.txt"
systemctl is-active --quiet rkaiq_3A.service
[ "$(pgrep -fc '^/usr/bin/rkaiq_3A_server([[:space:]]|$)')" -eq 1 ]
! fuser /dev/video5 /dev/video14 >/dev/null 2>&1
[ -z "$(ss -H -ltn 'sport = :8554 or sport = :8555')" ]
date -Is > "$b/start.txt"
sha256sum "$app/evrec" "$app/tools/run_camera_demo.sh" > "$b/inputs.sha256"
dmesg > "$b/dmesg-before.txt"
nohup env RTSP_PORT_OVERRIDE=8554 sh "$app/tools/run_camera_demo.sh" imx415-isp manual "$b/imx" > "$b/imx-console.log" 2>&1 </dev/null &
echo "$!" > "$b/imx.pid"
disown
nohup env RTSP_PORT_OVERRIDE=8555 sh "$app/tools/run_camera_demo.sh" ov8858-isp manual "$b/ov" > "$b/ov-console.log" 2>&1 </dev/null &
echo "$!" > "$b/ov.pid"
disown
echo "started $b"
