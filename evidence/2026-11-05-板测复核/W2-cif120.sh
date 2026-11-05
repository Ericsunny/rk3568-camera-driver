#!/bin/sh
# Pre-registered OV8858 CIF bounded capture; directory must already exist.
set -u
if [ "$#" -ne 1 ] || [ ! -d "$1" ]; then echo 'usage: sh W2-cif120.sh EXISTING_PRIVATE_DIR' >&2; exit 2; fi
run=$1
for file in cif120.raw cif-first.raw W2.capture.stdout W2.capture.stderr; do
    if [ -e "$run/$file" ]; then echo "refuse overwrite: $run/$file" >&2; exit 2; fi
done
echo 'START_UTC'; date -u +%Y-%m-%dT%H:%M:%SZ
echo 'START_UPTIME'; cat /proc/uptime
echo 'FORMAT_BEFORE'
fmt=$(v4l2-ctl -d /dev/video0 --get-fmt-video) || exit 2
printf '%s\n' "$fmt"
printf '%s\n' "$fmt" | grep -Eq 'Width/Height[[:space:]]*:[[:space:]]*1632/1224' || exit 2
printf '%s\n' "$fmt" | grep -Eq "Pixel Format[[:space:]]*:[[:space:]]*'BG10'" || exit 2
printf '%s\n' "$fmt" | grep -Eq 'Bytes per Line[[:space:]]*:[[:space:]]*2048' || exit 2
printf '%s\n' "$fmt" | grep -Eq 'Size Image[[:space:]]*:[[:space:]]*2506752' || exit 2
echo 'COMMAND: timeout 30s v4l2-ctl -d /dev/video0 --stream-no-query --stream-mmap=3 --stream-count=120 --stream-to=<run>/cif120.raw'
timeout 30s v4l2-ctl -d /dev/video0 --stream-no-query --stream-mmap=3 --stream-count=120 --stream-to="$run/cif120.raw" >"$run/W2.capture.stdout" 2>"$run/W2.capture.stderr"
rc=$?
echo "CAPTURE_EXIT=$rc"
echo 'CAPTURE_END_UTC'; date -u +%Y-%m-%dT%H:%M:%SZ
echo 'CAPTURE_END_UPTIME'; cat /proc/uptime
echo 'CAPTURE_STDOUT'; cat "$run/W2.capture.stdout"
echo 'CAPTURE_STDERR'; cat "$run/W2.capture.stderr"
if [ "$rc" -ne 0 ]; then echo 'RESULT=INVALID_CAPTURE'; exit 2; fi
bytes=$(wc -c < "$run/cif120.raw") || exit 2
echo "TOTAL_BYTES=$bytes"
if [ "$bytes" -ne 300810240 ]; then echo 'RESULT=INVALID_SIZE'; exit 2; fi
head -c 2506752 "$run/cif120.raw" > "$run/cif-first.raw" || exit 2
echo 'FRAME_SIZE'; wc -c "$run/cif-first.raw"
echo 'MD5'; md5sum "$run/cif120.raw" "$run/cif-first.raw"
echo 'SHA256'; sha256sum "$run/cif120.raw" "$run/cif-first.raw"
echo 'RESULT=CAPTURED_ANALYSIS_PENDING'
