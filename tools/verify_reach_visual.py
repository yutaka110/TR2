"""Audit real received-image control runs; does not claim G1-06 or network efficacy."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("invalid name")
    repo = Path(__file__).resolve().parents[1]
    root = repo / "artifacts/reach_g1_visual_20260921"
    output = root / "verification" / args.name
    output.mkdir(parents=True, exist_ok=False)
    cases = [
        ("t1_forward", ["--task", "T1", "--duration", "6"]),
        ("t2_approach_align", ["--task", "T2", "--duration", "12"]),
        ("t2_left", ["--task", "T2", "--duration", "6", "--initial-y", "0.15", "--initial-yaw", "0.174532925"]),
        ("t2_right", ["--task", "T2", "--duration", "6", "--initial-y", "-0.15", "--initial-yaw", "-0.174532925"]),
        ("software_stale", ["--task", "T2", "--duration", "4", "--encoder", "software"]),
        ("loss_idr", ["--task", "T2", "--duration", "4", "--diagnostic-drop-frame", "20"]),
    ]
    report = {"executable_sha256": hashlib.sha256((root / "bin/Release/GE3.exe").read_bytes()).hexdigest(),
              "scope": "G1-04 local diagnostic adapter; no reverse UDP; not G1-06", "cases": [], "passed": False}
    def require(ok, message):
        if not ok:
            raise AssertionError(message)
    try:
        for name, options in cases:
            invocation_name = args.name + "_" + name
            command = [sys.executable, str(repo / "tools/run_reach_g1.py"), "--stage", "visual_control", "--name", invocation_name, *options]
            result = subprocess.run(command, cwd=repo, capture_output=True)
            (output / (name + ".stdout.txt")).write_bytes(result.stdout)
            (output / (name + ".stderr.txt")).write_bytes(result.stderr)
            require(result.returncode == 0, name + ": app failed")
            invocation = root / "runs" / invocation_name
            session = next((invocation / "sessions").iterdir())
            read = lambda n: json.loads((session / n).read_text(encoding="utf-8"))
            rows = lambda n: list(csv.DictReader((session / n).open(encoding="utf-8", newline="")))
            transport, summary = read("robot_video_summary.json"), read("visual_control_summary.json")
            launch = json.loads((invocation / "launch.json").read_text(encoding="utf-8"))
            require(launch["executable_sha256"] == report["executable_sha256"], "binary changed")
            require(transport["validation_passed"] and transport["identity_errors"] == 0, name + ": video identity")
            observations = {int(r["frame_id"]): r for r in rows("observations.csv")}
            audit = {int(r["frame_id"]): r for r in rows("decoded_audit.csv")}
            evaluation = {int(r["frame_id"]): r for r in rows("capture_evaluation.csv")}
            commands = rows("commands.csv")
            max_position = max_yaw = 0.
            allowance_exceeded = 0
            for frame, obs in observations.items():
                require(obs["capture_us"] == audit[frame]["capture_us"], "capture identity changed")
                if obs["valid"] != "1":
                    continue
                require(audit[frame]["identity_match"] == "1", "unmatched image recognized")
                require(int(obs["received_us"]) - int(obs["capture_us"]) <= 200000, "stale image recognized")
                truth = evaluation[frame]
                error = math.hypot(float(obs["x_m"]) - float(truth["x_m"]), float(obs["y_m"]) - float(truth["y_m"]))
                yaw_error = abs(math.remainder(float(obs["yaw_rad"]) - float(truth["yaw_rad"]), 2 * math.pi))
                max_position = max(max_position, error)
                max_yaw = max(max_yaw, yaw_error)
                allowance_exceeded += error > float(obs["position_error_m"]) or yaw_error > float(obs["yaw_error_rad"])
            for i, cmd in enumerate(commands, 1):
                require(int(cmd["sequence"]) == i, "command sequence")
                now, capture = int(cmd["generated_us"]), int(cmd["source_capture_us"])
                require(int(cmd["valid_until_us"]) - now == 100000, "command lifetime")
                v, w = float(cmd["v_m_s"]), float(cmd["w_rad_s"])
                require(0 <= v <= .300001 and abs(w) <= .800001, "unbounded command")
                if v != 0 or w != 0 or cmd["estimated_complete"] == "1":
                    obs = observations[int(cmd["source_frame_id"])]
                    require(obs["valid"] == "1" and obs["capture_us"] == cmd["source_capture_us"] and obs["stream_id"] == cmd["source_stream_id"], "command provenance")
                    require(int(obs["received_us"]) <= now and 0 <= now - capture <= 200000, "future/stale command source")
                if cmd["reason"] in {"no_image", "observation_expired", "stale_at_receive", "marker_missing", "ambiguous_pose", "pose_jump"}:
                    require(v == 0 and w == 0, "invalid observation must stop")
            indexed = {int(c["sequence"]): c for c in commands}
            for applied in rows("local_applied_commands.csv"):
                sequence = int(applied["sequence"])
                if not sequence:
                    require(float(applied["v_m_s"]) == 0 and float(applied["w_rad_s"]) == 0, "startup movement")
                    continue
                source = indexed[sequence]
                live = int(applied["applied_us"]) <= int(source["valid_until_us"])
                require(int(applied["applied_us"]) >= int(source["generated_us"]), "applied future command")
                require(applied["live"] == str(int(live)), "local deadline check")
                for key in ("v_m_s", "w_rad_s"):
                    require(abs(float(applied[key]) - (float(source[key]) if live else 0)) < 1e-9, "local application differs from command")
            if name == "software_stale":
                require(any(o["reason"] == "stale_at_receive" for o in observations.values()), "software delay not observed")
            else:
                require(summary["recognized"] > 0 and summary["moving_commands"] > 0, "no recognition-driven command")
            if name == "loss_idr":
                require(any(c["reason"] == "observation_expired" for c in commands), "loss did not cause age stop")
                require(any(int(c["source_frame_id"]) >= 45 and float(c["v_m_s"]) > 0 for c in commands), "no post-IDR control recovery")
            item = {"name": name, "session": str(session.relative_to(repo)), "transport": transport, "control": summary,
                    "max_position_error_m": max_position, "max_yaw_error_rad": max_yaw, "initial_allowance_exceeded": allowance_exceeded,
                    "command_states": sorted({c["state"] for c in commands}), "command_reasons": sorted({c["reason"] for c in commands})}
            report["cases"].append(item)
            print(json.dumps({"case": name, "recognized": summary["recognized"], "max_position_error_m": max_position,
                              "allowance_exceeded": allowance_exceeded}, ensure_ascii=False), flush=True)
        require(all(c["initial_allowance_exceeded"] == 0 for c in report["cases"]), "INITIAL error allowance exceeded; inspect each case")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
