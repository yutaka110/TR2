"""Run the existing engine in isolated directories; this is not a Reach-RT task test."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from datetime import datetime


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--scenario", choices=["baseline", "loss10", "burst"], default="baseline")
    parser.add_argument("--seed", type=int, default=101)
    parser.add_argument("--duration", type=float, default=20)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--encoder", choices=["auto", "software", "hardware"], default="auto")
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("name must contain only letters, digits, underscore or hyphen")
    repo = Path(__file__).resolve().parents[1]
    root = repo / "artifacts/reach_g0_20260921"
    exe = root / "bin/Release/GE3.exe"
    if not exe.is_file():
        parser.error("Build GE3 first with tools/build_reach_g0.ps1")
    run = root / "runs" / args.name
    run.mkdir(parents=True, exist_ok=False)  # Never overwrite a prior experiment.
    for asset_dir in ("Resources", "config"):
        if (repo / asset_dir).is_dir():
            shutil.copytree(repo / asset_dir, run / asset_dir)
    prefix = {"baseline": "Baseline", "loss10": "10% loss", "burst": "Burst loss"}[args.scenario]
    expected_scenario = prefix + " / QoE/Deadline Adaptive / Hybrid / FEC g4"
    overrides = {
        "RNVP_CODEC": "h264", "RNVP_DISABLE_CAMERA": "1",
        "TR2_NETWORK_MODE": "loopback", "TR2_NETWORK_EXPERIMENT_AUTO": "1",
        "TR2_NETWORK_EXPERIMENT_SCENARIO": expected_scenario,
        "TR2_NETWORK_EXPERIMENT_DURATION_SEC": str(args.duration),
        "TR2_NETWORK_EXPERIMENT_WARMUP_SEC": "5",
        "TR2_NETWORK_SIM_SEED": str(args.seed), "RNVP_STARTUP_TRACE": "1",
        "RNVP_H264_GOP": "30", "RNVP_H264_ENCODER": args.encoder,
        "RNVP_PRESENT_SYNC_INTERVAL": "0",
    }
    env = {k: v for k, v in os.environ.items()
           if not k.upper().startswith(("RNVP_", "TR2_NETWORK_"))}
    env.update(overrides)
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=repo)
    (run / "source.patch").write_bytes(diff)
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=repo).decode("utf-8").split("\0")
    files = {p: sha256(repo / p) for p in tracked if p and (repo / p).is_file()
             and (Path(p).suffix in (".cpp", ".h", ".hlsl", ".vcxproj", ".sln", ".lib"))}
    write_json(run / "source_hashes.json", files)
    manifest = {
        "kind": "existing_engine_g0_baseline_not_reach_rt", "started_at": datetime.now().astimezone().isoformat(),
        "executable": str(exe), "executable_sha256": sha256(exe),
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "source_diff_sha256": hashlib.sha256(diff).hexdigest(), "environment_overrides": overrides,
        "working_directory": str(run), "arguments": vars(args),
        "runner_sha256": sha256(Path(__file__)),
    }
    write_json(run / "manifest.json", manifest)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    started = time.monotonic()
    with (run / "stdout.txt").open("wb") as stdout, (run / "stderr.txt").open("wb") as stderr:
        proc = subprocess.Popen([str(exe)], cwd=run, env=env, stdout=stdout, stderr=stderr, startupinfo=startup)
        try:
            code = proc.wait(timeout=args.timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            proc.kill()  # Only the process created by this run.
            code = proc.wait()
            timed_out = True
    manifest.update(exit_code=code, timed_out=timed_out, elapsed_s=time.monotonic() - started,
                    finished_at=datetime.now().astimezone().isoformat())
    candidates = [p for p in (run / "logs").glob("network_*.csv")
                  if not p.name.startswith(("network_summary_", "network_events_"))]
    observed = set()
    for path in candidates:
        with path.open(encoding="utf-8-sig", newline="") as f:
            for row in csv.DictReader(f):
                if row.get("scenarioName") not in (None, "", "Auto"):
                    observed.add(row["scenarioName"])
    manifest["observed_scenarios"] = sorted(observed)
    manifest["scenario_match"] = observed == {expected_scenario}
    manifest["log_files"] = sorted(str(p.relative_to(run)) for p in (run / "logs").glob("*"))
    write_json(run / "manifest.json", manifest)
    print(json.dumps({"run": str(run), "exit_code": code, "timed_out": timed_out,
                      "scenario_match": manifest["scenario_match"], "observed": sorted(observed)}, ensure_ascii=False), flush=True)
    return 0 if code == 0 and not timed_out and manifest["scenario_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
