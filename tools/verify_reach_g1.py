"""Exercise actual G1 dispatch, validation, session isolation, and log consistency."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
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
    exe = root / "bin/Release/GE3.exe"
    binary_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    source = json.loads((repo / "config/reach_rt_g1_foundation.json").read_text(encoding="utf-8"))
    source["duration_s"] = 0.5
    base = json.dumps(source)
    cases = [
        ("T1", base, "reach_rt", 0),
        ("T2", base.replace('"T1"', '"T2"'), "reach_rt", 0),
        ("unknown_mode", base, "reach_typo", 2),
        ("duplicate", base.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1'), "reach_rt", 2),
        ("unknown_key", base.replace('"seed": 101', '"seed_typo": 101'), "reach_rt", 2),
        ("wrong_type", base.replace('"camera": 30', '"camera": "30"'), "reach_rt", 2),
        ("zero_rate", base.replace('"camera": 30', '"camera": 0'), "reach_rt", 2),
        ("unsupported_stage", base.replace('"foundation"', '"closed_loop"'), "reach_rt", 2),
        ("truncated", base[:-1], "reach_rt", 2),
        ("missing_config", None, "reach_rt", 2),
    ]

    def run(case):
        name, text, mode, expected = case
        folder = out / name
        folder.mkdir()
        config = folder / "input.json"
        if text is not None:
            config.write_text(text, encoding="utf-8")
        env = {key: value for key, value in os.environ.items()
               if not key.upper().startswith(("RNVP_", "TR2_NETWORK_", "TR2_REACH_", "TR2_RESEARCH_"))}
        env.update(TR2_RESEARCH_MODE=mode, TR2_REACH_CONFIG=str(config),
                   TR2_REACH_OUTPUT_ROOT=str(folder / "sessions"), TR2_REACH_HEADLESS="1")
        result = subprocess.run([str(exe)], cwd=repo, env=env, capture_output=True, timeout=15)
        (folder / "stdout.txt").write_bytes(result.stdout)
        (folder / "stderr.txt").write_bytes(result.stderr)
        assert result.returncode == expected, (name, result.returncode, result.stderr)
        summaries = list((folder / "sessions").glob("*/summary.json"))
        record = {"case": name, "exit_code": result.returncode, "expected_exit_code": expected, "passed": True}
        if expected == 0:
            assert len(summaries) == 1
            directory = summaries[0].parent
            summary = json.loads(summaries[0].read_text(encoding="utf-8"))
            manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
            events = [json.loads(line) for line in (directory / "events.jsonl").read_text(encoding="utf-8").splitlines()]
            assert summary["status"] == "foundation_completed"
            assert summary["task_result"] == "not_run" and not summary["closed_loop_validated"]
            assert summary["clock_tick_counts"] == {"physics": 50, "camera": 15, "control": 10}
            assert manifest["executable_sha256"] == binary_hash
            assert manifest["input_config_sha256"] == hashlib.sha256(config.read_bytes()).hexdigest()
            assert manifest["effective_config_sha256"] == hashlib.sha256((directory / "config.effective.json").read_bytes()).hexdigest()
            assert [e["seq"] for e in events] == list(range(1, len(events) + 1))
            assert all(e["session_id"] == summary["session_id"] for e in events)
            assert all(a["monotonic_us"] <= b["monotonic_us"] for a, b in zip(events, events[1:]))
            assert all(e["elapsed_us"] == e["monotonic_us"] - manifest["monotonic_origin_us"] for e in events)
            for domain, count in summary["clock_tick_counts"].items():
                assert sum(e["count"] for e in events if e["event"] == "clock_tick" and e["detail"] == domain) == count
            record["session_id"] = summary["session_id"]
        else:
            assert not summaries, name
            assert b"Reach-RT start/run failed" in result.stderr
        return record

    records = [run(case) for case in cases]
    # Independent process sessions can coexist without reusing identity or logs.
    with ThreadPoolExecutor(max_workers=2) as pool:
        concurrent = list(pool.map(run, [("concurrent_a", base, "reach_rt", 0), ("concurrent_b", base, "reach_rt", 0)]))
    records.extend(concurrent)
    ids = [r["session_id"] for r in records if "session_id" in r]
    assert len(ids) == len(set(ids))
    report = {"executable_sha256": binary_hash, "passed_cases": len(records), "cases": records,
              "scope": "G1-01 foundation only; no video/control efficacy claim"}
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
