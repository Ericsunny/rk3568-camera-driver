#!/bin/sh
# Pre-registered IMX415 two-frame capture. Output directory must already exist.
set -u
if [ "$#" -ne 1 ] || [ ! -d "$1" ]; then
    echo 'usage: sh W1-twoframes.sh EXISTING_PRIVATE_DIR' >&2
    exit 2
fi
run=$1
for file in two.nv12 frame-aa.nv12 frame-ab.nv12 capture.stdout capture.stderr; do
    if [ -e "$run/$file" ]; then echo "refuse overwrite: $run/$file" >&2; exit 2; fi
done
echo 'START_UTC'; date -u +%Y-%m-%dT%H:%M:%SZ
echo 'START_UPTIME'; cat /proc/uptime
echo 'FORMAT_BEFORE'
fmt=$(v4l2-ctl -d /dev/video5 --get-fmt-video) || exit 2
printf '%s\n' "$fmt"
printf '%s\n' "$fmt" | grep -Eq 'Width/Height[[:space:]]*:[[:space:]]*3840/2160' || exit 2
printf '%s\n' "$fmt" | grep -Eq "Pixel Format[[:space:]]*:[[:space:]]*'NV12'" || exit 2
printf '%s\n' "$fmt" | grep -Eq 'Bytes per Line[[:space:]]*:[[:space:]]*3840' || exit 2
printf '%s\n' "$fmt" | grep -Eq 'Size Image[[:space:]]*:[[:space:]]*12441600' || exit 2
echo 'COMMAND: timeout 20s v4l2-ctl -d /dev/video5 --stream-no-query --stream-mmap=3 --stream-count=2 --stream-to=<run>/two.nv12'
timeout 20s v4l2-ctl -d /dev/video5 --stream-no-query --stream-mmap=3 --stream-count=2 --stream-to="$run/two.nv12" >"$run/capture.stdout" 2>"$run/capture.stderr"
rc=$?
echo "CAPTURE_EXIT=$rc"
echo 'CAPTURE_STDOUT'; cat "$run/capture.stdout"
echo 'CAPTURE_STDERR'; cat "$run/capture.stderr"
if [ "$rc" -ne 0 ]; then echo 'RESULT=INVALID_CAPTURE'; exit 2; fi
bytes=$(wc -c < "$run/two.nv12") || exit 2
echo "TOTAL_BYTES=$bytes"
if [ "$bytes" -ne 24883200 ]; then echo 'RESULT=INVALID_SIZE'; exit 2; fi
split -b 12441600 --additional-suffix=.nv12 "$run/two.nv12" "$run/frame-" || exit 2
echo 'FRAME_SIZES'; wc -c "$run/frame-aa.nv12" "$run/frame-ab.nv12"
echo 'MD5'; md5sum "$run/two.nv12" "$run/frame-aa.nv12" "$run/frame-ab.nv12"
echo 'SHA256'; sha256sum "$run/two.nv12" "$run/frame-aa.nv12" "$run/frame-ab.nv12"
cmp -s "$run/frame-aa.nv12" "$run/frame-ab.nv12"
cmp_rc=$?
echo "CMP_EXIT=$cmp_rc"
echo 'END_UTC'; date -u +%Y-%m-%dT%H:%M:%SZ
echo 'END_UPTIME'; cat /proc/uptime
if [ "$cmp_rc" -eq 1 ]; then echo 'RESULT=DIFFERENT'; exit 0; fi
if [ "$cmp_rc" -eq 0 ]; then echo 'RESULT=IDENTICAL_INCONCLUSIVE'; exit 3; fi
echo 'RESULT=INVALID_COMPARE'; exit 2
