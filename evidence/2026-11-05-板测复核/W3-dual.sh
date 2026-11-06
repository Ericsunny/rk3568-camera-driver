#!/bin/sh
# Fixed 8 s singles followed by simultaneous acquisition; 3A stays running.
set -u
if [ "$#" -ne 1 ] || [ ! -d "$1" ] || [ ! -x "$1/capbench" ]; then
    echo 'usage: sh W3-dual.sh PRIVATE_DIR_WITH_CAPBENCH' >&2; exit 2
fi
run=$1
for name in cif-single isp-single cif-dual isp-dual; do
    for ext in stdout stderr exit timing; do
        if [ -e "$run/$name.$ext" ]; then echo "refuse overwrite: $name.$ext" >&2; exit 2; fi
    done
done
measure() {
    name=$1; dev=$2; width=$3; height=$4; fmt=$5
    { echo START_UTC; date -u +%Y-%m-%dT%H:%M:%SZ; echo START_UPTIME; cat /proc/uptime;
      echo FORMAT_BEFORE; v4l2-ctl -d "$dev" --get-fmt-video; } > "$run/$name.timing"
    timeout 20s "$run/capbench" "$dev" 8 "$width" "$height" "$fmt" > "$run/$name.stdout" 2> "$run/$name.stderr"
    rc=$?
    printf '%s\n' "$rc" > "$run/$name.exit"
    { echo END_UTC; date -u +%Y-%m-%dT%H:%M:%SZ; echo END_UPTIME; cat /proc/uptime;
      echo FORMAT_AFTER; v4l2-ctl -d "$dev" --get-fmt-video; } >> "$run/$name.timing"
    return "$rc"
}
echo 'COMMANDS: capbench /dev/video0 8 1632 1224 BG10; capbench /dev/video5 8 3840 2160 NV12; then both concurrently; each timeout 20s'
measure cif-single /dev/video0 1632 1224 BG10 || exit 2
measure isp-single /dev/video5 3840 2160 NV12 || exit 2
measure cif-dual /dev/video0 1632 1224 BG10 & p1=$!
measure isp-dual /dev/video5 3840 2160 NV12 & p2=$!
wait "$p1"; r1=$?
wait "$p2"; r2=$?
echo "DUAL_EXITS=$r1,$r2"
for name in cif-single isp-single cif-dual isp-dual; do
    echo "=== $name timing ==="; cat "$run/$name.timing"
    echo "=== $name stdout ==="; cat "$run/$name.stdout"
    echo "=== $name stderr ==="; cat "$run/$name.stderr"
    echo "=== $name exit ==="; cat "$run/$name.exit"
done
if [ "$r1" -ne 0 ] || [ "$r2" -ne 0 ]; then exit 2; fi
