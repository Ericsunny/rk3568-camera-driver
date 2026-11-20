"""Short RK3568 acceptance. Supply a built repository and a NEW private test path."""
import csv
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def until(predicate, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.1)
    raise RuntimeError("condition timed out")


def resources(pid):
    p = Path("/proc") / str(pid)
    rss = re.search(r"VmRSS:\s+(\d+)", (p / "status").read_text())
    return {"fds": len(list((p / "fd").iterdir())), "rss_kib": int(rss[1])}


src, case = (Path(s).resolve() for s in sys.argv[1:3])
case.mkdir(mode=0o700)
archive = case / "archive"
archive.mkdir(mode=0o700)
old = archive / "run-00000001"
current = archive / "run-00000002"
env = dict(os.environ, CAMERA_RETENTION_ROOT=str(archive), CAMERA_SWEEP_SEC="2")
# Existing media smoke checks occupancy, strict decode and the manual receipt.
cmd = [sys.executable, str(src / "evidence/2026-11-20-entrance-release/smoke.py"),
       str(src), str(old), "manual"]
first = json.loads(subprocess.check_output(cmd, env=env, text=True))
report, = old.glob("*.delivery.json")
stem = report.name.split(".")[0]
pin = old / ".keep"
pin.touch()
for extra in ([], ["--selfpath"]):
    node = subprocess.check_output([str(src / "resolve_camera"), "imx415-isp", *extra], text=True).strip()
    users = subprocess.run(["fuser", node], capture_output=True, text=True)
    assert users.returncode in (0, 1) and all(
        (Path("/proc") / pid / "comm").read_text().strip() == "rkaiq_3A_server"
        for pid in users.stdout.split())
port = "18596"
assert not subprocess.check_output(["ss","-H","-ltn","sport = :" + port])
before = subprocess.check_output(["dmesg"]).splitlines()
env.update(CAMERA_MAX_MIB="1", CAMERA_TARGET_MIB="0", RTSP_PORT_OVERRIDE=port,
           FRAME_METRICS=str(current / "main.csv"))
progress = case / "rtsp.progress"
with (case / "entrance.log").open("x") as log:
    service = subprocess.Popen([str(src / "camera_demo"), "imx415-isp", "entrance", str(archive / "next")],
                               env=env, stdout=log, stderr=subprocess.STDOUT)
    viewer = None
    try:
        until(lambda: (current / "producer.pid").exists() and subprocess.check_output(
            ["ss","-H","-ltn","sport = :" + port]).strip(), 15)
        viewer = subprocess.Popen(["ffmpeg","-nostdin","-hide_banner","-v","error","-xerror",
                                   "-rtsp_transport","tcp","-analyzeduration","0","-probesize","32768",
                                   "-i","rtsp://127.0.0.1:" + port + "/live","-frames:v","150",
                                   "-progress",str(progress),"-f","null","-"], stderr=log)
        def frame():
            if not progress.exists():
                return 0
            marks = re.findall(r"^frame=(\d+)$", progress.read_text(), re.M)
            return int(marks[-1]) if marks else 0
        until(lambda: frame() > 0 and "target unmet" in (case / "entrance.log").read_text(errors="replace"), 15)
        producer = int((current / "producer.pid").read_text())
        pre = {str(name): resources(pid) for name,pid in (("supervisor",service.pid),("producer",producer))}
        assert report.exists() and frame() < 150
        start_frame = frame()
        pin.unlink()
        until(lambda: not list(old.glob(stem + ".*")), 8)
        end_frame = frame()
        assert viewer.poll() is None and 0 < start_frame <= end_frame < 150
        assert viewer.wait(timeout=20) == 0 and frame() == 150
        post = {str(name): resources(pid) for name,pid in (("supervisor",service.pid),("producer",producer))}
    finally:
        if viewer and viewer.poll() is None:
            viewer.kill()
            viewer.wait()
        if service.poll() is None:
            service.send_signal(signal.SIGTERM)
        status = service.wait(timeout=40)
after = subprocess.check_output(["dmesg"]).splitlines()
with (current / "main.csv").open() as f:
    rows = list(csv.DictReader(f))
with (current / "main.csv.side.csv").open() as f:
    accepted = sum(r["result"] == "accepted" for r in csv.DictReader(f))
seq = [int(row["driver_sequence"]) for row in rows]
result = dict(manual=first, entrance_exit=status, enrolled_next=current.name, viewer_frames=frame(),
              viewer_frames_at_unpin_and_observed_completion=[start_frame,end_frame],
              main_frames=len(seq), main_completed=sum(int(r["completed"]) for r in rows),
              main_sequence_breaks=sum(b != a+1 for a,b in zip(seq,seq[1:])),
              side_accepted=accepted, before=pre, after=post, prior_receipt_group_removed=True,
              kernel_log_contiguous=after[:len(before)] == before,
              kernel_alerts=[s.decode(errors="replace") for s in after[len(before):]
                             if re.search(b"crc errors|kernel panic|BUG:|Oops:",s,re.I)])
print(json.dumps(result))
assert not status and result["main_completed"] == len(seq) >= 150 and not result["main_sequence_breaks"]
assert accepted and result["kernel_log_contiguous"] and not result["kernel_alerts"]
