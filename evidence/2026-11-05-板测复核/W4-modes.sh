#!/bin/sh
# Run instrumented historical zc_npu copy in three isolated directories.
set -u
if [ "$#" -ne 1 ] || [ ! -d "$1" ] || [ ! -x "$1/zc_npu_review" ]; then
    echo 'usage: sh W4-modes.sh PRIVATE_DIR_WITH_BINARY' >&2; exit 2
fi
run=$1
for mode in a b c; do
    if [ -e "$run/$mode" ]; then echo "refuse overwrite: $run/$mode" >&2; exit 2; fi
done
for mode in a b c; do
    mkdir "$run/$mode" || exit 2
    echo "MODE=$mode START_UTC"; date -u +%Y-%m-%dT%H:%M:%SZ
    echo 'START_UPTIME'; cat /proc/uptime
    echo "COMMAND: RUNS=1 timeout 60s <run>/zc_npu_review $mode 30 /dev/video5"
    (cd "$run/$mode" && RUNS=1 timeout 60s "$run/zc_npu_review" "$mode" 30 /dev/video5) > "$run/$mode/stdout" 2> "$run/$mode/stderr"
    rc=$?
    printf '%s\n' "$rc" > "$run/$mode/exit"
    echo "MODE=$mode END_UTC"; date -u +%Y-%m-%dT%H:%M:%SZ
    echo 'END_UPTIME'; cat /proc/uptime
    echo 'STDOUT'; cat "$run/$mode/stdout"
    echo 'STDERR'; cat "$run/$mode/stderr"
    echo "EXIT=$rc"
    if [ -f "$run/$mode/s4_det.ppm" ]; then echo 'IMAGE_MD5'; md5sum "$run/$mode/s4_det.ppm"; fi
    if [ "$rc" -ne 0 ]; then echo 'RESULT=STOP_ON_FAILURE'; exit 2; fi
    if ! grep -q 'W4 completed=30 requested=30' "$run/$mode/stdout"; then echo 'RESULT=INCOMPLETE'; exit 2; fi
    if grep -E '(^|[[:space:]])(fail|failed)([[:space:]]|$)|W4 RGA failed' "$run/$mode/stdout" "$run/$mode/stderr"; then echo 'RESULT=ERROR_LOG'; exit 2; fi
done
echo 'RESULT=MEASUREMENTS_RECORDED; NATIVE_OUTPUT_CORRECTNESS_NOT_TESTED'
