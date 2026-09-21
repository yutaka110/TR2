"""Actual H264/RNVP/UDP pixel and PTS validation; no robot-control efficacy claim."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def rows(path):
    with path.open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum(): parser.error("invalid name")
    repo = Path(__file__).resolve().parents[1]
    root = repo / "artifacts/reach_g1_robot_20260921"
    out = root / "verification" / args.name
    out.mkdir(parents=True, exist_ok=False)
    cases = [
        ("auto_stop", ["--duration", "15"]),
        ("software_delay", ["--duration", "4", "--encoder", "software", "--task", "T2"]),
        ("auto_repeat_a", ["--duration", "3", "--task", "T2", "--initial-y", "0.15", "--initial-yaw", "0.17"]),
        ("auto_repeat_b", ["--duration", "3", "--task", "T2", "--initial-y", "0.15", "--initial-yaw", "0.17"]),
        ("software_tail_only", ["--duration", "0.1", "--encoder", "software"]),
        ("missing_p", ["--duration", "3", "--diagnostic-drop-frame", "20"]),
    ]
    report = {"executable_sha256": hashlib.sha256((root / "bin/Release/GE3.exe").read_bytes()).hexdigest(), "cases": []}
    session_ids, streams, trajectories = set(), set(), {}
    for name, options in cases:
        run_name = args.name + "_" + name
        command = [sys.executable, str(repo / "tools/run_reach_g1.py"), "--stage", "robot_video", "--name", run_name, "--timeout", "30", *options]
        result = subprocess.run(command, cwd=repo, capture_output=True, timeout=45)
        (out / (name + "_launcher.txt")).write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, (name, result.stdout.decode("utf-8", "replace"))
        folders = list((root / "runs" / run_name / "sessions").iterdir())
        assert len(folders) == 1
        folder = folders[0]
        summary = json.loads((folder / "robot_video_summary.json").read_text())
        session = json.loads((folder / "summary.json").read_text())
        manifest = json.loads((folder / "manifest.json").read_text())
        captures, encoded, decoded = [rows(folder / p) for p in ("captures.csv", "encoded.csv", "decoded_audit.csv")]
        assert summary["validation_passed"] and summary["identity_errors"] == 0
        assert not summary["closed_loop_validated"] and session["task_result"] == "not_run"
        assert manifest["executable_sha256"] == report["executable_sha256"]
        assert manifest["session_id"] not in session_ids
        session_ids.add(manifest["session_id"])
        stream = captures[0]["stream_id"]
        assert stream not in streams
        streams.add(stream)
        capture_map = {row["frame_id"]: row for row in captures}
        assert len(capture_map) == len(captures) == len(encoded)
        for row in encoded:
            original = capture_map[row["frame_id"]]
            assert int(row["output_pts_100ns"]) == int(original["capture_us"]) * 10
            assert row["capture_us"] == original["capture_us"]
            assert int(row["encoder_output_us"]) >= int(row["capture_us"])
        for row in decoded:
            original = capture_map[row["frame_id"]]
            assert row["frame_id"] == row["pixel_id"]
            assert row["stream_id"] == row["pixel_stream"] == stream
            assert row["capture_us"] == row["expected_capture_us"] == original["capture_us"] == row["matched_source_pts_us"]
            assert int(row["output_pts_100ns"]) == int(original["capture_us"]) * 10
            assert row["identity_match"] == row["pixel_valid"] == "1" and row["duplicate"] == "0"
            assert float(row["age_ms"]) >= 0
        if name == "missing_p":
            assert summary["sent"] == summary["captured"] - 1
            assert all(int(row["frame_id"]) < 20 or int(row["frame_id"]) >= 45 for row in decoded)
            assert any(int(row["frame_id"]) >= 45 for row in decoded)
        else:
            assert summary["captured"] == summary["encoded"] == summary["decoded"] == summary["matched"]
        if name.startswith("software"):
            assert summary["encoder_drain_outputs"] > 0 and not summary["async_hardware"]
        if name == "software_tail_only": assert summary["captured"] == summary["encoder_drain_outputs"] == 3
        world = rows(folder / "world.csv")
        if name == "auto_stop":
            assert float(world[1200]["v_m_s"]) > 0
            assert float(world[-1]["v_m_s"]) == 0 and 1.7 < float(world[-1]["x_m"]) < 1.9
        if name.startswith("auto_repeat"):
            trajectories[name] = world
        record = dict(case=name, passed=True, directory=str(folder.relative_to(repo)), **summary)
        report["cases"].append(record)
        (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(name, "PASS", summary["decoded"], "matched", flush=True)
    assert trajectories["auto_repeat_a"] == trajectories["auto_repeat_b"], "simulation reset/replay differs"
    report["passed_cases"] = len(cases)
    report["identities_checked"] = sum(c["matched"] for c in report["cases"])
    report["repeat_trajectory_equal"] = True
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "cases"}), flush=True)


if __name__ == "__main__":
    main()
