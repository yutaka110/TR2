"""Fault-inject copies of a real successful session; preserve original evidence."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import tempfile

from verify_reach_closed_loop import adjudicate, read, rows, save
from verify_reach_command import require


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    result = read(args.report)
    case = next(c for c in result["cases"] if c["passed"])
    original = Path(case["session"])
    checks = []
    adjudicate(original, case)
    checks.append(dict(name="real_success", passed=True))
    faults = [
        ("wrong_initial_pose", "world.csv", 0, "y_m", ".4"),
        ("premature_success", "world.csv", 0, "success", "1"),
        ("fabricated_hold", "world.csv", 0, "goal_hold_s", "1"),
        ("altered_motion", "world.csv", 500, "x_m", "1.23"),
        ("contact_hidden_by_success", "world.csv", 5999, "collision", "1"),
        ("bypassed_udp", "udp_applied_commands.csv", 500, "sequence", "99999"),
        ("truncated_window", "summary.json", "clock_tick_counts", "physics", 5999),
        ("missing_video", "robot_video_summary.json", None, "matched", 1799),
    ]
    # Keep automatically cleaned test copies under the explicitly selected evidence folder.
    temporary_root = args.report.parent.resolve()
    with tempfile.TemporaryDirectory(prefix="reach_g106_audit_", dir=temporary_root) as temporary:
        require(Path(temporary).resolve().is_relative_to(temporary_root), "test copies escaped evidence directory")
        for name, filename, index, key, value in faults:
            target = Path(temporary) / name / original.name
            target.mkdir(parents=True)
            for pattern in ("*.json", "*.csv"):
                for file in original.glob(pattern):
                    shutil.copyfile(file, target / file.name)
            path = target / filename
            if path.suffix == ".csv":
                data = rows(path)
                data[index][key] = value
                with path.open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(data[0]))
                    writer.writeheader()
                    writer.writerows(data)
            else:
                data = read(path)
                (data if index is None else data[index])[key] = value
                save(path, data)
            try:
                adjudicate(target, case)
            except AssertionError as error:
                checks.append(dict(name=name, passed=True, rejection=str(error)))
            else:
                checks.append(dict(name=name, passed=False))
    report = dict(passed=all(c["passed"] for c in checks), checks=checks, source_session=str(original))
    save(args.report.parent / "auditor_fault_tests.json", report)
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
