"""Short target-board check. Requires the built applications and an idle camera."""
import csv
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

root, out = map(Path, sys.argv[1:3])
mode = sys.argv[3]
if mode not in ("manual", "entrance") or out.exists() or not out.is_absolute():
    sys.exit("expected a new absolute run directory and manual or entrance")
port = "18596"

def read(*args):
    return subprocess.check_output(args, text=True)

def records(name):
    with (out / name).open() as stream:
        return list(csv.DictReader(stream))

for opt in ([], ["--selfpath"]) if mode == "entrance" else ([],):
    node = read(str(root / "resolve_camera"), "imx415-isp", *opt).strip()
    users = subprocess.run(["fuser", node], capture_output=True, text=True)
    if users.returncode not in (0, 1) or any(Path("/proc", pid, "comm").read_text().strip()
                                             != "rkaiq_3A_server" for pid in users.stdout.split()):
        sys.exit("capture endpoint busy or unavailable")
if read("ss", "-H", "-ltn", "sport = :" + port).strip():
    sys.exit("test port busy")
before = read("dmesg").splitlines()
env = dict(os.environ, RTSP_PORT_OVERRIDE=port, FRAME_METRICS=str(out / "main.csv"))
with open(str(out) + ".log", "x") as log:
    service = subprocess.Popen([str(root / "camera_demo"), "imx415-isp", mode, str(out)],
                               env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(75):
            if service.poll() is not None:
                sys.exit("service exited before RTSP listen")
            if read("ss", "-H", "-ltn", "sport = :" + port).strip():
                break
            time.sleep(0.2)
        if mode == "manual":
            time.sleep(4)
            (out / "trigger").touch()
        progress = subprocess.check_output(
            ["ffmpeg", "-nostdin", "-hide_banner", "-v", "error", "-xerror", "-rtsp_transport",
             "tcp", "-i", "rtsp://127.0.0.1:" + port + "/live", "-frames:v", "90",
             "-progress", "pipe:1", "-f", "null", "-"], stderr=log, text=True, timeout=25)
    finally:
        if service.poll() is None:
            service.send_signal(signal.SIGTERM)
        status = service.wait(timeout=35)
after = read("dmesg").splitlines()
rows = records("main.csv")
seq = [int(row["driver_sequence"]) for row in rows]
args = (out / "argv.txt").read_text().splitlines()
receipts = [json.loads(p.read_text()) for p in out.glob("*.delivery.json")]
side = records("main.csv.side.csv") if mode == "entrance" else []
result = dict(mode=mode, service_exit=status, captured=len(seq),
              completed=sum(int(row["completed"]) for row in rows),
              sequence_breaks=sum(b != a+1 for a, b in zip(seq, seq[1:])),
              rtsp_frames=90 if re.search(r"^frame=90$", progress, re.M) else 0,
              accepted_results=len({(r["epoch"], r["frame_seq"]) for r in side if r["result"]=="accepted"}),
              delivery=[(r["status"], r["video_frames"]) for r in receipts],
              kernel_log_contiguous=after[:len(before)] == before,
              kernel_alerts=[line for line in after[len(before):]
                             if re.search("crc errors|kernel panic|BUG:|Oops:", line, re.I)])
print(json.dumps(result, ensure_ascii=False))
ok = (not status and result["completed"] == len(seq) and len(seq) >= 90 and
      not result["sequence_breaks"] and result["rtsp_frames"] == 90 and
      result["kernel_log_contiguous"] and not result["kernel_alerts"])
if mode == "entrance":
    ok = ok and result["accepted_results"] > 0 and all(
        opt in args for opt in ("--detect-selfpath", "--selfpath-contiguous", "--selfpath-poll"))
else:
    ok = ok and any(s == "complete" and n > 0 for s,n in result["delivery"])
sys.exit(0 if ok else 1)
