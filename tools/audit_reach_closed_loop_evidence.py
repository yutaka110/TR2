"""Cross-check source/config identity and every raw capture/decoded identity row."""
import argparse
import hashlib
import math
from pathlib import Path

from verify_reach_closed_loop import read, rows, save
from verify_reach_command import require


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    matrix = read(args.report)
    plan = read(args.report.parent / "plan.json")
    require(plan["verifier_sha256"] == digest(repo / "tools/verify_reach_closed_loop.py"), "matrix verifier changed")
    report = dict(passed=False, matrix_complete=matrix["complete"], matrix_passed=matrix["passed"],
                  scope="source/config/raw identity integrity; allowance coverage reported separately", cases=[], allowance_violations=[])
    try:
        for case in matrix["cases"]:
            session = Path(case["session"])
            invocation = Path(case["invocation"])
            config = read(invocation / "requested_config.json")
            require(config["stage"] == "command_udp" and config["task"] == case["task"], "wrong mode/task")
            require(config["duration_s"] == 60 and config["encoder"] == "auto" and config["seed"] == 101, "wrong conditions")
            require(config["tick_rates_hz"] == dict(physics=100, camera=30, control=20) and config["max_catchup_steps"] == 5, "clock relaxation")
            require(config["command_link"] == dict(scenario="normal"), "non-ideal command scenario")
            require(config["world"] == dict(initial_y_m=case["y_m"], initial_yaw_rad=math.radians(case["yaw_deg"]), corridor_width_m=.75), "changed world")
            hashes = read(invocation / "source_hashes.json")
            for relative, expected in hashes.items():
                if relative.startswith(("research/", "network/")) and Path(relative).suffix in (".cpp", ".h"):
                    require(digest(repo / relative) == expected, "native source changed: " + relative)
            require(read(invocation / "launch.json")["executable_sha256"] == matrix["executable_sha256"], "different executable")
            item = dict(id=case["id"], config_and_source_match=True, complete_trial=case["passed"])
            if not case["passed"]:
                report["cases"].append(item)
                continue
            captures = {int(r["frame_id"]): r for r in rows(session / "captures.csv")}
            decoded = rows(session / "decoded_audit.csv")
            require(set(captures) == set(range(1, 1801)) and len(decoded) == 1800, "capture/decoded IDs")
            seen = set()
            for d in decoded:
                fid = int(d["frame_id"])
                require(fid not in seen, "duplicate decoded image")
                seen.add(fid)
                c = captures[fid]
                require(d["frame_id"] == d["pixel_id"] and c["stream_id"] == d["stream_id"] == d["pixel_stream"], "pixel identity differs")
                require(c["capture_us"] == d["capture_us"] == d["expected_capture_us"] == d["matched_source_pts_us"], "capture identity differs")
                require(c["encoder_input_pts_100ns"] == d["output_pts_100ns"] == str(int(c["capture_us"])*10), "PTS differs")
                require(d["identity_match"] == d["pixel_valid"] == "1" and d["duplicate"] == "0", "invalid image identity")
            observations = rows(session / "observations.csv")
            truth = {r["frame_id"]: r for r in rows(session / "capture_evaluation.csv")}
            max_position = max_yaw = 0.
            exceeded = 0
            for o in observations:
                if o["valid"] != "1":
                    continue
                t = truth[o["frame_id"]]
                error = math.hypot(float(o["x_m"])-float(t["x_m"]), float(o["y_m"])-float(t["y_m"]))
                angle = abs(math.remainder(float(o["yaw_rad"])-float(t["yaw_rad"]), 2*math.pi))
                max_position, max_yaw = max(max_position, error), max(max_yaw, angle)
                outside = error > float(o["position_error_m"]) or angle > float(o["yaw_error_rad"])
                exceeded += outside
                if outside:
                    report["allowance_violations"].append(dict(case=case["id"], frame_id=int(o["frame_id"]),
                        position_error_m=error, position_allowance_m=float(o["position_error_m"]),
                        yaw_error_deg=math.degrees(angle), yaw_allowance_rad=float(o["yaw_error_rad"])))
            item.update(raw_image_identities_checked=len(decoded), max_position_error_m=max_position,
                        max_yaw_error_deg=math.degrees(max_yaw), initial_allowance_exceeded=exceeded)
            report["cases"].append(item)
        report["initial_allowance_model_passed"] = not report["allowance_violations"]
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    save(args.report.parent / "evidence_audit.json", report)
    print(report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
