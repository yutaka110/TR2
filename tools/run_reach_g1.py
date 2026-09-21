"""Launch the G1-01 research foundation with isolated, attributable evidence."""
from __future__ import annotations
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, data: object) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True, help="Unique directory name for this invocation")
    parser.add_argument("--stage", choices=["foundation", "robot_video"], default="foundation")
    parser.add_argument("--build-name", help="Build artifact directory name")
    parser.add_argument("--duration", type=float, help="Override observation interval in seconds")
    parser.add_argument("--task", choices=["T1", "T2"])
    parser.add_argument("--encoder", choices=["auto", "software"], default="auto")
    parser.add_argument("--initial-y", type=float, default=0)
    parser.add_argument("--initial-yaw", type=float, default=0)
    parser.add_argument("--diagnostic-drop-frame", type=int, default=0, help="Omit one complete AU before UDP; validation only")
    parser.add_argument("--packet-trace", action="store_true", help="Record sendto/recvfrom packet identities")
    parser.add_argument("--show", action="store_true", help="Display dashboard; close it to return")
    parser.add_argument("--timeout", type=float, default=90, help="Headless timeout only")
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("name must contain only letters, digits, underscore or hyphen")
    repo = Path(__file__).resolve().parents[1]
    build_name = args.build_name or ("reach_g1_robot_20260921" if args.stage == "robot_video" else "reach_g1_20260921")
    if not build_name.replace("_", "").replace("-", "").isalnum():
        parser.error("invalid build directory name")
    root = repo / "artifacts" / build_name
    exe = root / "bin/Release/GE3.exe"
    if not exe.is_file():
        parser.error("Build Release with OutputName=reach_g1_20260921 first")
    invocation = root / "runs" / args.name
    invocation.mkdir(parents=True, exist_ok=False)
    config_name = "reach_rt_g1_robot.json" if args.stage == "robot_video" else "reach_rt_g1_foundation.json"
    config = json.loads((repo / "config" / config_name).read_text(encoding="utf-8"))
    if args.stage == "robot_video":
        config["encoder"] = args.encoder
        config["world"]["initial_y_m"] = args.initial_y
        config["world"]["initial_yaw_rad"] = args.initial_yaw
    if args.duration is not None:
        config["duration_s"] = args.duration
    if args.task:
        config["task"] = args.task
    config_path = invocation / "requested_config.json"
    write_json(config_path, config)
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("RNVP_", "TR2_NETWORK_", "TR2_REACH_", "TR2_RESEARCH_"))}
    overrides = {"TR2_RESEARCH_MODE": "reach_rt", "TR2_REACH_CONFIG": str(config_path),
                 "TR2_REACH_OUTPUT_ROOT": str(invocation / "sessions"),
                 "TR2_REACH_HEADLESS": "0" if args.show else "1"}
    env.update(overrides)
    if args.packet_trace:
        overrides["TR2_REACH_PACKET_TRACE"] = "1"
        env.update(overrides)
    if args.diagnostic_drop_frame:
        overrides["TR2_REACH_DIAGNOSTIC_DROP_FRAME"] = str(args.diagnostic_drop_frame)
        env.update(overrides)
    patch = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=repo)
    (invocation / "source.patch").write_bytes(patch)
    # Include new untracked research files, which git diff alone would omit.
    candidates = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=repo)
    source_hashes = {}
    snapshot = invocation / "research_source"
    snapshot.mkdir()
    for relative in set(candidates.decode("utf-8").split("\0")):
        path = repo / relative
        if relative and path.is_file() and path.suffix in {".h", ".cpp", ".vcxproj", ".props", ".py", ".json"}:
            source_hashes[relative] = digest(path)
            if relative.startswith(("research/", "network/", "config/reach_rt_g1")) or relative == "tools/run_reach_g1.py":
                saved = snapshot / relative
                saved.parent.mkdir(parents=True, exist_ok=True)
                saved.write_bytes(path.read_bytes())
    write_json(invocation / "source_hashes.json", source_hashes)
    manifest = {"started_at": datetime.now().astimezone().isoformat(), "arguments": vars(args),
                "executable_sha256": digest(exe), "launcher_sha256": digest(Path(__file__)),
                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
                "source_diff_sha256": hashlib.sha256(patch).hexdigest(), "environment_overrides": overrides,
                "task_execution": "scripted_integration_validation" if args.stage == "robot_video" else "not_implemented_in_G1_01"}
    write_json(invocation / "launch.json", manifest)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 5 if args.show else subprocess.SW_HIDE  # Win32 SW_SHOW = 5
    with (invocation / "stdout.txt").open("wb") as stdout, (invocation / "stderr.txt").open("wb") as stderr:
        process = subprocess.Popen([str(exe)], cwd=invocation, env=env, startupinfo=startup, stdout=stdout, stderr=stderr)
        manifest["process_id"] = process.pid
        write_json(invocation / "launch.json", manifest)
        try:
            code = process.wait(timeout=None if args.show else args.timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            process.kill()  # Only this launcher's child process.
            code = process.wait()
            timed_out = True
    sessions = list((invocation / "sessions").glob("*/summary.json"))
    result = json.loads(sessions[0].read_text(encoding="utf-8")) if len(sessions) == 1 else None
    manifest.update(finished_at=datetime.now().astimezone().isoformat(), exit_code=code,
                    timed_out=timed_out, session_summary=result)
    write_json(invocation / "launch.json", manifest)
    print(json.dumps({"invocation": str(invocation), "exit_code": code, "timed_out": timed_out,
                      "summary": result}, ensure_ascii=False), flush=True)
    return 0 if code == 0 and not timed_out and result and result["status"] == args.stage + "_completed" else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
