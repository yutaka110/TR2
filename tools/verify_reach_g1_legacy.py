"""Check that the G1 executable still dispatches to the existing RNVP mode."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--build-name", default="reach_g1_20260921")
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("use a simple directory name")
    repo = Path(__file__).resolve().parents[1]
    if not args.build_name.replace("_", "").replace("-", "").isalnum(): parser.error("invalid build name")
    root = repo / "artifacts" / args.build_name
    out = root / "verification" / args.name
    out.mkdir(parents=True, exist_ok=False)
    for directory in ("Resources", "config"):
        shutil.copytree(repo / directory, out / directory)
    exe = root / "bin/Release/GE3.exe"
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("RNVP_", "TR2_NETWORK_", "TR2_REACH_", "TR2_RESEARCH_"))}
    scenario = "Baseline / QoE/Deadline Adaptive / Hybrid / FEC g4"
    overrides = dict(TR2_RESEARCH_MODE="legacy", RNVP_CODEC="h264", RNVP_DISABLE_CAMERA="1",
                     TR2_NETWORK_MODE="loopback", TR2_NETWORK_EXPERIMENT_AUTO="1",
                     TR2_NETWORK_EXPERIMENT_SCENARIO=scenario, TR2_NETWORK_EXPERIMENT_DURATION_SEC="8",
                     TR2_NETWORK_EXPERIMENT_WARMUP_SEC="2", TR2_NETWORK_SIM_SEED="101",
                     RNVP_STARTUP_TRACE="1", RNVP_PRESENT_SYNC_INTERVAL="0")
    env.update(overrides)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    with (out / "stdout.txt").open("wb") as stdout, (out / "stderr.txt").open("wb") as stderr:
        process = subprocess.Popen([str(exe)], cwd=out, env=env, stdout=stdout, stderr=stderr, startupinfo=startup)
        try:
            code = process.wait(timeout=40)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise
    files = list((out / "logs").glob("network_summary_*.csv"))
    assert code == 0 and len(files) == 1, (code, files)
    with files[0].open(encoding="utf-8-sig", newline="") as file:
        rows = list(csv.DictReader(file))
    assert len(rows) == 1 and float(rows[0]["displayedFrames"]) > 0
    with next((out / "logs").glob("h264_receive_trace_*.csv")).open(encoding="utf-8-sig", newline="") as file:
        decoded = sum(row["decodeStatus"] == "Decoded" for row in csv.DictReader(file))
    assert decoded > 0
    report = {"exit_code": code, "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "environment_overrides": overrides, "sampled_decoded_rows": decoded,
              "displayed_frames": rows[0]["displayedFrames"], "passed": True,
              "limitation": "legacy dispatch smoke only, not a performance comparison or PTS fix validation"}
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
