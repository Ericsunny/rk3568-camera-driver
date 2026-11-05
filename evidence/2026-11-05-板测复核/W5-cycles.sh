#!/bin/sh
# Stop on the first failed cycle; never stop or restart the 3A service.
set -u
if [ "$#" -ne 1 ] || [ ! -d "$1" ] || [ ! -x "$1/st_burn" ]; then
    echo 'usage: sh W5-cycles.sh PRIVATE_DIR_WITH_ST_BURN' >&2; exit 2
fi
run=$1
if [ -e "$run/cycles" ]; then echo 'refuse overwrite cycles directory' >&2; exit 2; fi
mkdir "$run/cycles" || exit 2
echo 'COMMAND: 100 times timeout 8s <run>/st_burn -d /dev/video5 -w 3840 -h 2160 -c 1; stop on first failure'
echo START_UTC; date -u +%Y-%m-%dT%H:%M:%SZ
echo START_UPTIME; cat /proc/uptime
i=1
while [ "$i" -le 100 ]; do
    name=$(printf '%03d' "$i")
    timeout 8s "$run/st_burn" -d /dev/video5 -w 3840 -h 2160 -c 1 > "$run/cycles/$name.stdout" 2> "$run/cycles/$name.stderr"
    rc=$?
    printf '%s\n' "$rc" > "$run/cycles/$name.exit"
    printf 'CYCLE_INDEX=%s EXIT=%s ' "$i" "$rc"
    cat "$run/cycles/$name.stdout"
    if [ "$rc" -ne 0 ] || ! grep -q 'cycles=1 ok=1 fail=0 ' "$run/cycles/$name.stdout" || [ -s "$run/cycles/$name.stderr" ]; then
        echo "RESULT=STOP_ON_FAILURE cycle=$i"; cat "$run/cycles/$name.stderr"; exit 2
    fi
    i=$((i + 1))
done
echo END_UTC; date -u +%Y-%m-%dT%H:%M:%SZ
echo END_UPTIME; cat /proc/uptime
echo 'RESULT=100_FIRST_FRAMES_RECEIVED'
