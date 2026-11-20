"""Short board probe. All files, including deletion fixtures, live in a new TemporaryDirectory."""
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess as sp
import sys
import tempfile
import time

def check(ok, message):
    if not ok:
        raise RuntimeError(message)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def used(path):
    return sum(p.stat().st_blocks * 512 for p in path.iterdir() if p.is_file())

def wait_for(path, proc):
    end = time.monotonic() + 15
    while not path.exists():
        check(proc.poll() is None and time.monotonic() < end, "missing " + path.name)
        time.sleep(0.05)

def snapshot(pid):
    proc = Path("/proc", str(pid))
    rss = next(l for l in (proc / "status").read_text().splitlines() if l.startswith("VmRSS:"))
    return {"rss_kib": int(rss.split()[1]), "fds": len(list((proc / "fd").iterdir()))}

def session(dirpath, service, video, packet_rows):
    dirpath.mkdir()
    counts = []
    with (dirpath / "service.log").open("w") as log:
        stop = dirpath / "stop-fixture"
        proc = sp.Popen([str(service), str(dirpath), "--", "/bin/sh", "-c",
                         'while [ ! -f "$1" ]; do sleep 0.1; done', "fixture", str(stop)],
                        stdout=log, stderr=log)
        try:
            wait_for(dirpath / "producer.pid", proc)
            for i in range(1, 5):
                stem = dirpath / ("event-%02d" % i)
                shutil.copyfile(video, stem.with_suffix(".h264"))
                with stem.with_suffix(".packets.csv").open("w") as f:
                    writer = csv.writer(f, lineterminator="\n")
                    writer.writerow(["frame_seq","packet_index","media_ms","capture_ms","bytes","decodable_start"])
                    for n, p in enumerate(packet_rows):
                        writer.writerow([n,0,1000+100*n,1000+100*n,p["size"],int("K" in p["flags"])])
                meta = {"schema":2, "timestamp_source":"v4l2-monotonic", "bytes":video.stat().st_size,
                        "write_error":False, "first_ts_ms":1000, "last_ts_ms":1900, "event_id":i}
                tmp = stem.with_suffix(".pending.json")
                tmp.write_text(json.dumps(meta))
                tmp.rename(stem.with_suffix(".json"))
                receipt = stem.with_suffix(".delivery.json")
                wait_for(receipt, proc)
                info = json.loads(receipt.read_text())
                check(info["status"] == "complete" and info["video_frames"] == 10, "delivery failed")
                counts.append(dict(events=i, allocated_bytes=used(dirpath), **snapshot(proc.pid)))
        finally:
            stop.touch()
            rc = proc.wait(timeout=12)
            stop.unlink()
    check(rc == 0, "unexpected supervisor exit %d" % rc)
    return counts, rc

def valid_archive(path):
    # Test fixture has a private root, no concurrent writer, and exactly four closed events.
    if not (path / "sealed").is_file() or (path / "keep").exists():
        return False
    expected = {"sealed", "producer.pid", "service.log"}
    for i in range(1, 5):
        stem = "event-%02d" % i
        for ext in (".json", ".h264", ".packets.csv", ".mp4", ".delivery.json"):
            expected.add(stem + ext)
    if {p.name for p in path.iterdir()} != expected or any(
            p.is_symlink() or not p.is_file() for p in path.iterdir()):
        return False
    for i in range(1, 5):
        stem = path / ("event-%02d" % i)
        r = json.loads(stem.with_suffix(".delivery.json").read_text())
        if r.get("status") != "complete":
            return False
        for ext, field in ((".h264","source_sha256"), (".json","metadata_sha256"),
                           (".packets.csv","packet_log_sha256"), (".mp4","output_sha256")):
            if sha(stem.with_suffix(ext)) != r.get(field):
                return False
    return True

def plan(root, budget):
    sessions = sorted(root.iterdir())  # zero-padded persistent session numbers
    remaining = sum(used(p) for p in sessions)
    candidates = []
    for p in sessions:
        if remaining <= budget:
            break
        if valid_archive(p):
            candidates.append(p)
            remaining -= used(p)
    return candidates, remaining

def run(base):
    tools = Path(sys.argv[1]).resolve()
    video = base / "sample.h264"
    sp.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=160x120:rate=10:duration=1",
            "-c:v","libx264","-threads","1","-bf","0","-f","h264",str(video)],check=True)
    packets=json.loads(sp.check_output(["ffprobe","-v","error","-show_packets","-show_entries",
               "packet=size,flags","-of","json",str(video)]))["packets"]
    check(len(packets)==10, "sample requires ten packets")
    root = base / "sessions"
    root.mkdir()
    observations = {}
    for n in ("001","002"):
        p=root/n
        observations[n]=session(p,tools/"camera_service",video,packets)
        (p/"sealed").touch()
    for name in ("003","004","005","006","007"):
        shutil.copytree(root/"002", root/name)
    (root/"003"/"keep").touch()
    (root/"004"/"sealed").unlink()
    (root/"005"/"event-01.pending.json").touch()
    (root/"006"/"event-01.mp4").write_bytes(b"damaged")
    (root/"007"/"event-01.delivery.json").write_text('{"status":"failed"}')
    check([p.name for p in sorted(root.iterdir()) if valid_archive(p)] == ["001","002"],"eligibility")
    before=sum(used(p) for p in root.iterdir())
    oldest=used(root/"001")
    budget=before-oldest
    selected, expected=plan(root,budget)
    check([p.name for p in selected]==["001"],"oldest selection")
    check(plan(root,0)[1]>0, "protected sessions must prevent an impossible quota")
    # Only delete the freshly generated known archive, never accept a caller-supplied data directory.
    trash=base/"retired-001"
    (root/"001").rename(trash)
    shutil.rmtree(trash)
    after=sum(used(p) for p in root.iterdir())
    check(after==expected and after==budget and not plan(root,budget)[0],"reclaim or replay")
    return dict(kernel=os.uname().release, arch=os.uname().machine, samples=observations,
                allocated_before=before, budget=budget, allocated_after=after, reclaimed=before-after,
                retired=["001"], retained=sorted(p.name for p in root.iterdir()),
                second_pass_actions=0, impossible_quota_protected=True)

if __name__=="__main__":
    with tempfile.TemporaryDirectory(prefix="retention-synthetic-") as tmp:
        print(json.dumps(run(Path(tmp)),indent=2))
