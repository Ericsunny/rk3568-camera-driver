#!/bin/sh
# Final-system entry: a fixed profile, never changes services or boot files.
set -eu

if [ "$#" -ne 2 ] || { [ "$1" != detect ] && [ "$1" != manual ]; }; then
    echo "Usage: $0 detect|manual NEW_RUN_DIRECTORY" >&2
    echo "detect: class-0/person events; manual: touch RUN/trigger after warm-up" >&2
    exit 2
fi
mode=$1
run_dir=$2
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$root/config/final-demo.env"
binary=${EVREC_BIN:-$root/evrec}
for tool in pgrep fuser sha256sum ss python3 ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; exit 1; }
done
case "$run_dir" in /*) ;; *) echo 'RUN_DIRECTORY must be absolute' >&2; exit 2;; esac
[ -x "$binary" ] || { echo "missing executable: $binary" >&2; exit 1; }
[ -c "$VIDEO" ] || { echo "missing video device: $VIDEO" >&2; exit 1; }
# A service's "active" alone does not prove a live daemon (forking unit may
# report active/exited with MainPID=0). Validate one actual process as well.
count=$(pgrep -f '^/usr/bin/rkaiq_3A_server([[:space:]]|$)' | wc -l)
[ "$count" -eq 1 ] || { echo 'expected exactly one running rkaiq_3A_server' >&2; exit 1; }
if fuser "$VIDEO" >/dev/null 2>&1; then
    echo "capture device already in use: $VIDEO" >&2
    exit 1
fi
if [ -n "$(ss -H -ltn "sport = :$RTSP_PORT")" ]; then
    echo "RTSP port already in use: $RTSP_PORT" >&2
    exit 1
fi
if [ "$mode" = detect ]; then
    printf '%s  %s\n' "$MODEL_SHA256" "$MODEL" | sha256sum -c -
fi
# A new directory makes all output paths distinct from existing recordings.
mkdir -m 700 -- "$run_dir"
uname -a > "$run_dir/kernel.txt"
date -u > "$run_dir/board-clock.txt"
sha256sum "$binary" "$root/config/final-demo.env" > "$run_dir/inputs.sha256"
set -- "$binary" -d "$VIDEO" -w "$WIDTH" -h "$HEIGHT" \
    --pre "$PRE_SEC" --post "$POST_SEC" --ring-mb "$RING_MB" --kbps "$KBPS" \
    --max-event-sec "$MAX_EVENT_SEC" --cooldown "$COOLDOWN_SEC" \
    --disk-min-mb "$DISK_MIN_MB" --recover 1 --max-events "${MAX_EVENTS:-0}" \
    --rtsp "$RTSP_PORT" --rtsp-clients "$RTSP_CLIENTS" \
    --rtsp-deadline-s "$RTSP_DEADLINE" --osd -o "$run_dir/event"
if [ "$mode" = detect ]; then
    set -- "$@" --detect --model "$MODEL" --det-score "$DET_SCORE" --det-frames "$DET_FRAMES"
else
    # Leave the path absent until the operator triggers it. evrec treats an
    # already-existing file's first observed mtime as a trigger too.
    set -- "$@" --trigger "$run_dir/trigger"
fi
printf '%s\n' "$@" > "$run_dir/argv.txt"
exec python3 "$root/tools/camera_service.py" "$run_dir" -- "$@"
