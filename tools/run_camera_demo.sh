#!/bin/sh
# Final-system entry: a fixed profile, never changes services or boot files.
set -eu

profile=
if [ "$#" -eq 3 ]; then
    profile=$1
    shift
fi
if [ "$#" -ne 2 ] || { [ "$1" != detect ] && [ "$1" != manual ]; } ||
   { [ -n "$profile" ] && [ "$profile" != imx415-isp ] && [ "$profile" != ov8858-isp ]; }; then
    echo "Usage: $0 [imx415-isp|ov8858-isp] detect|manual NEW_RUN_DIRECTORY" >&2
    echo "detect: class-0/person events; manual: touch RUN/trigger after warm-up" >&2
    exit 2
fi
mode=$1
run_dir=$2
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$root/config/final-demo.env"
if [ -n "$profile" ]; then
    . "$root/config/$profile.env"
    command -v media-ctl >/dev/null 2>&1 || { echo 'missing media-ctl' >&2; exit 1; }
    VIDEO=$(python3 "$root/tools/resolve_camera.py" "$profile") || exit 1
fi
# Allow independent instances to listen on different ports while keeping the
# configured default for existing single-camera invocations.
RTSP_PORT=${RTSP_PORT_OVERRIDE:-$RTSP_PORT}
case "$RTSP_PORT" in
    ''|*[!0-9]*|??????*) echo 'invalid RTSP port' >&2; exit 2 ;;
esac
if [ "$RTSP_PORT" -lt 1 ] || [ "$RTSP_PORT" -gt 65535 ]; then
    echo 'invalid RTSP port' >&2
    exit 2
fi
binary=${EVREC_BIN:-$root/evrec}
ENCODER_INPUT=${ENCODER_INPUT:-dmabuf}
if [ -z "${NPU_INPUT:-}" ]; then
    if [ "$mode" = detect ]; then NPU_INPUT=dmabuf; else NPU_INPUT=copy; fi
fi
DETECT_EVERY=${DETECT_EVERY:-1}
case "$DETECT_EVERY" in
    ''|*[!0-9]*|????*) echo 'DETECT_EVERY must be 1..120' >&2; exit 2 ;;
esac
if [ "$DETECT_EVERY" -lt 1 ] || [ "$DETECT_EVERY" -gt 120 ]; then
    echo 'DETECT_EVERY must be 1..120' >&2
    exit 2
fi
case "$ENCODER_INPUT" in
    copy|dmabuf) ;;
    *) echo 'ENCODER_INPUT must be copy or dmabuf' >&2; exit 2 ;;
esac
case "$NPU_INPUT" in
    copy|dmabuf) ;;
    *) echo 'NPU_INPUT must be copy or dmabuf' >&2; exit 2 ;;
esac
if [ "$NPU_INPUT" = dmabuf ] && [ "$mode" != detect ]; then
    echo 'NPU_INPUT=dmabuf requires detect mode' >&2
    exit 2
fi
for tool in pgrep fuser sha256sum ss python3 ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; exit 1; }
done
case "$run_dir" in /*) ;; *) echo 'RUN_DIRECTORY must be absolute' >&2; exit 2;; esac
[ -x "$binary" ] || { echo "missing executable: $binary" >&2; exit 1; }
if [ "$ENCODER_INPUT" = dmabuf ] &&
   ! "$binary" --help | grep -q -- '--encoder-input'; then
    echo 'evrec binary does not support dmabuf input' >&2
    exit 1
fi
if [ "$NPU_INPUT" = dmabuf ] &&
   ! "$binary" --help | grep -q -- '--npu-input'; then
    echo 'evrec binary does not support dmabuf NPU input' >&2
    exit 1
fi
if [ "$mode" = detect ] && [ "$DETECT_EVERY" -gt 1 ] &&
   ! "$binary" --help | grep -q -- '--detect-every'; then
    echo 'evrec binary does not support detection sampling' >&2
    exit 1
fi
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
if [ -n "$profile" ]; then
    printf '%s\n' "$profile" > "$run_dir/profile.txt"
    sha256sum "$root/config/$profile.env" >> "$run_dir/inputs.sha256"
fi
set -- "$binary" -d "$VIDEO" -w "$WIDTH" -h "$HEIGHT" \
    --fps "$FPS" --gop "$GOP" --pre "$PRE_SEC" --post "$POST_SEC" \
    --ring-mb "$RING_MB" --kbps "$KBPS" \
    --max-event-sec "$MAX_EVENT_SEC" --cooldown "$COOLDOWN_SEC" \
    --disk-min-mb "$DISK_MIN_MB" --recover 1 --max-events "${MAX_EVENTS:-0}" \
    --rtsp "$RTSP_PORT" --rtsp-clients "$RTSP_CLIENTS" \
    --rtsp-deadline-s "$RTSP_DEADLINE" --osd -o "$run_dir/event"
if [ "$ENCODER_INPUT" = dmabuf ]; then
    set -- "$@" --encoder-input dmabuf
fi
if [ "$mode" = detect ]; then
    set -- "$@" --detect --model "$MODEL" --det-score "$DET_SCORE" --det-frames "$DET_FRAMES"
    if [ "$DETECT_EVERY" -gt 1 ]; then
        set -- "$@" --detect-every "$DETECT_EVERY"
    fi
    if [ "$NPU_INPUT" = dmabuf ]; then
        set -- "$@" --npu-input dmabuf
    fi
else
    # Leave the path absent until the operator triggers it. evrec treats an
    # already-existing file's first observed mtime as a trigger too.
    set -- "$@" --trigger "$run_dir/trigger"
fi
printf '%s\n' "$@" > "$run_dir/argv.txt"
exec python3 "$root/tools/camera_service.py" "$run_dir" -- "$@"
