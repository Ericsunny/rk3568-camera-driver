"""Linux host regression for the native archive helper; all inputs are disposable."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def fixture(run, serial):
    stem = f"event_20261120-010203-{serial:02d}"
    files = {ext: run / (stem + ext) for ext in
             (".json", ".h264", ".mp4", ".packets.csv", ".delivery.json")}
    receipt = {"schema": 1, "status": "complete", "source_json": files[".json"].name,
               "source_video": files[".h264"].name, "output_video": files[".mp4"].name}
    for ext, field in ((".json", "metadata_sha256"), (".h264", "source_sha256"),
                       (".mp4", "output_sha256"), (".packets.csv", "packet_log_sha256")):
        data = b"x" * (1024 * 1024 if ext == ".mp4" else 10)
        files[ext].write_bytes(data)
        receipt[field] = hashlib.sha256(data).hexdigest()
    files[".delivery.json"].write_text(json.dumps(receipt))
    return files


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        binary = tmp / "camera_retention"
        service = tmp / "camera_service"
        for exe in (binary, service):
            subprocess.run(["g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tools" / (exe.name + ".cpp")), "-o", str(exe)], check=True)
        root = tmp / "archive"
        root.mkdir(mode=0o700)
        env = dict(os.environ, CAMERA_RETENTION_ROOT=str(root), CAMERA_MAX_MIB="1", CAMERA_TARGET_MIB="0")
        def enroll(run):
            run.mkdir(mode=0o700)
            return subprocess.run([service, run, "--", "/bin/true"], env=env, capture_output=True)
        def sweep():
            return subprocess.run([binary, root], env=env, capture_output=True, check=True).stderr
        run = root / "run-00000002"
        assert enroll(run).returncode == 0
        assert enroll(root / "run-00000001").returncode != 0
        with (root / ".camera-owner.lock").open() as lk:
            fcntl.flock(lk, fcntl.LOCK_EX)
            assert enroll(root / "run-00000003").returncode != 0
        good, pinned, partial, corrupt, failed, linked, orphan, unknown = [
            fixture(run, n) for n in range(1, 9)]
        pinned[".json"].with_suffix(".keep").touch()
        partial[".json"].with_suffix(".pending.mp4").touch()
        corrupt[".mp4"].write_bytes(b"changed")
        failed[".delivery.json"].write_text('{"schema":1,"status":"failed"}')
        linked[".mp4"].unlink()
        linked[".mp4"].symlink_to(pinned[".mp4"])
        orphan[".delivery.json"].unlink()
        unknown[".json"].with_suffix(".operator-note").touch()
        unmanaged = fixture(root / "run-00000001", 9)
        (run / ".keep").touch()
        assert b"reclaimed" not in sweep()
        (run / ".keep").unlink()
        result = sweep()
        assert result.count(b"reclaimed") == 1 and b"target unmet" in result, result
        assert not any(p.exists() for p in good.values())
        for protected in (pinned, partial, corrupt, failed, linked, orphan, unknown, unmanaged):
            assert protected[".json"].exists() and protected[".mp4"].exists()
        interrupted = fixture(run, 10)
        interrupted[".delivery.json"].rename(interrupted[".json"].with_suffix(".retiring"))
        interrupted[".h264"].unlink()
        assert b"resumed" in sweep()
        assert not any(p.exists() for p in interrupted.values())
        assert not interrupted[".json"].with_suffix(".retiring").exists()
        damaged = fixture(run, 11)
        damaged[".delivery.json"].rename(damaged[".json"].with_suffix(".retiring"))
        damaged[".mp4"].write_bytes(b"modified during restart")
        assert b"resumed" not in sweep() and damaged[".json"].exists()
        assert b"reclaimed" not in sweep()
        assert subprocess.run([binary, root], env=dict(env, CAMERA_TARGET_MIB="1")).returncode != 0
    print("PASS native retention: admission, exclusion, pressure, protection, resume, idempotence")


if __name__ == "__main__":
    main()
