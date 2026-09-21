"""G1-06: preregister 18 poses, run full 60 s windows, independently audit outcomes.

No retries or selection of successful trials. A new invocation preserves old evidence.
The C++ engine runs the experiment; Python orchestrates and independently audits CSVs.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

from verify_reach_command import audit, require


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def rows(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def save(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")


def adjudicate(session, case):
    summary = read(session / "summary.json")
    require(summary["status"] == "command_udp_completed", "execution incomplete")
    require(summary["clock_tick_counts"] == dict(physics=6000, camera=1800, control=1200), "full 60 s clock counts")
    video = read(session / "robot_video_summary.json")
    require(all(video[k] == 1800 for k in ("captured", "encoded", "sent", "decoded", "matched")), "full video window")
    require(video["complete_no_loss_delivery"] and video["diagnostic_drop_frame"] == 0, "non-ideal video")
    causal = audit(session)
    require(causal["scenario"] == "normal" and causal["summary"]["statuses"] == {"accepted": 1200}, "normal command delivery")
    states = rows(session / "world.csv")
    applications = rows(session / "udp_applied_commands.csv")
    require(len(states) == 6000, "full physics window")
    first = states[0]
    require(abs(float(first["x_m"])) < 1e-12 and abs(float(first["y_m"])-case["y_m"]) < 1e-10
            and abs(float(first["yaw_rad"])-math.radians(case["yaw_deg"])) < 1e-10, "incorrect initial pose")
    hold = 0
    success_time = None
    previous = dict(x=0., y=case["y_m"], yaw=math.radians(case["yaw_deg"]), v=0., w=0.)
    min_clearance = float("inf")
    for tick, (row, application) in enumerate(zip(states, applications), 1):
        require(int(row["physics_tick"]) == tick and abs(float(row["simulation_s"])-tick/100) < 1e-8, "physics timeline")
        x, y, yaw, v, w = (float(row[k]) for k in ("x_m", "y_m", "yaw_rad", "v_m_s", "w_rad_s"))
        require(all(math.isfinite(n) for n in (x, y, yaw, v, w)), "nonfinite world")
        require(row["collision"] == row["out_of_bounds"] == "0", "collision or out of bounds")
        require(-.5 <= x <= 4.5 and abs(y) <= 2, "independent bounds")
        if case["task"] == "T1":
            clearance = min(math.hypot(x-max(0., min(3., x)), y-wall)-.2 for wall in (-.375, .375))
            min_clearance = min(min_clearance, clearance)
            require(clearance > 0, "independent wall contact")
        if success_time is None:
            # Reconstruct pre-terminal velocities: the C++ evaluator freezes v/w on success.
            target_v, target_w = float(application["v_m_s"]), float(application["w_rad_s"])
            dv = .006 if target_v < previous["v"] else .003
            raw_v = previous["v"] + max(-dv, min(dv, target_v-previous["v"]))
            raw_w = previous["w"] + max(-.016, min(.016, target_w-previous["w"]))
            turn, travel = (previous["w"]+raw_w)*.005, (previous["v"]+raw_v)*.005
            nx = previous["x"] + travel*math.cos(previous["yaw"]+turn*.5)
            ny = previous["y"] + travel*math.sin(previous["yaw"]+turn*.5)
            na = math.remainder(previous["yaw"]+turn, 2*math.pi)
            require(max(abs(x-nx), abs(y-ny), abs(yaw-na)) < 2e-9, "independent kinematic integration")
            in_goal = 3.20 <= x <= 3.35 if case["task"] == "T1" else math.hypot(x-2, y) <= .10 and abs(yaw) <= math.radians(5)
            hold = hold+1 if in_goal and raw_v <= .02 else 0
            require(abs(float(row["goal_hold_s"])-hold/100) < 1e-8, "independent goal hold")
            expected_success = hold >= 100
            require((row["success"] == "1") == expected_success, "success flag inconsistent with 1 s goal hold")
            if expected_success:
                success_time = tick/100
        else:
            require(row["success"] == "1" and v == w == 0, "terminal hold not retained")
            require(x == previous["x"] and y == previous["y"] and yaw == previous["yaw"], "terminal world drift")
        previous = dict(x=x, y=y, yaw=yaw, v=v, w=w)
    require(success_time is not None and states[-1]["timeout"] == "0", "task did not complete by 60 s")
    truth = {r["frame_id"]:r for r in rows(session / "capture_evaluation.csv")}
    max_position_error = max_yaw_error = 0.
    for o in rows(session / "observations.csv"):
        if o["valid"] != "1":continue
        t = truth[o["frame_id"]]
        error = math.hypot(float(o["x_m"])-float(t["x_m"]),float(o["y_m"])-float(t["y_m"]))
        yaw_error = abs(math.remainder(float(o["yaw_rad"])-float(t["yaw_rad"]),2*math.pi))
        max_position_error=max(max_position_error,error);max_yaw_error=max(max_yaw_error,yaw_error)
        require(error <= float(o["position_error_m"]) and yaw_error <= float(o["yaw_error_rad"]), "accepted image exceeds recorded pose allowance")
    return dict(success_s=success_time, final_pose=previous, minimum_wall_clearance_m=min_clearance if case["task"] == "T1" else None,
                max_position_error_m=max_position_error,max_yaw_error_deg=math.degrees(max_yaw_error),allowance_violations=0,
                pipeline_metrics=video.get("pipeline_metrics"),
                decoded_images=video["decoded"], recognized=video["recognized"], recognition_rejected=video["recognition_rejected"],
                encoder_mode=video["encoder_mode"], async_hardware=video["async_hardware"], physics_steps_audited=len(states),
                accepted_commands=causal["summary"]["statuses"]["accepted"], command_ip_bytes=causal["summary"]["attempted_ip_bytes"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--build-name", default="reach_g1_closed_loop_20260921")
    parser.add_argument("--stop-on-failure", action="store_true", help="Preserve failure and stop; never marks a partial matrix passed")
    args = parser.parse_args()
    for value in (args.name, args.build_name):
        if not value.replace("_", "").replace("-", "").isalnum():
            parser.error("invalid name")
    repo = Path(__file__).resolve().parents[1]
    root = repo / "artifacts" / args.build_name
    output = root / "verification" / args.name
    output.mkdir(parents=True, exist_ok=False)
    exe_hash = hashlib.sha256((root / "bin/Release/GE3.exe").read_bytes()).hexdigest()
    cases = [dict(id=f"{task}_{yi}_{ai}", task=task, y_m=yi*(.05 if task == "T1" else .15), yaw_deg=ai*(5 if task == "T1" else 10))
             for task in ("T1", "T2") for yi in (-1, 0, 1) for ai in (-1, 0, 1)]
    plan = dict(cases=cases, duration_s=60, encoder="auto", command_scenario="normal", task_deadline_s=60,
                success_hold_s=1, retries=0, stop_on_failure=args.stop_on_failure, executable_sha256=exe_hash,
                require_pose_allowance_coverage=True, require_pipeline_telemetry=True,
                verifier_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    save(output / "plan.json", plan)
    report = dict(passed=False, complete=False, executable_sha256=exe_hash, cases=[])
    for case in cases:
        print(json.dumps(dict(starting=case)), flush=True)
        item = dict(**case, passed=False)
        try:
            name = args.name + "_" + case["id"]
            result = subprocess.run([sys.executable, str(repo / "tools/run_reach_g1.py"), "--stage", "command_udp", "--name", name,
                                     "--build-name", args.build_name, "--task", case["task"], "--duration", "60", "--encoder", "auto",
                                     "--initial-y", str(case["y_m"]), "--initial-yaw", str(math.radians(case["yaw_deg"]))], cwd=repo, capture_output=True)
            (output / (case["id"] + ".stdout.txt")).write_bytes(result.stdout)
            (output / (case["id"] + ".stderr.txt")).write_bytes(result.stderr)
            invocation = root / "runs" / name
            item["invocation"] = str(invocation)
            launch = read(invocation / "launch.json")
            require(launch["executable_sha256"] == exe_hash, "binary changed")
            session = next((invocation / "sessions").iterdir())
            item["session"] = str(session)
            execution = read(session / "summary.json")
            item["execution"] = execution
            states = rows(session / "world.csv")
            if states:
                item["observed_final_state"] = states[-1]
                item["observed_success_s"] = next((float(s["simulation_s"]) for s in states if s["success"] == "1"), None)
            require(result.returncode == 0, "app failed: " + execution["reason"])
            metrics = read(session / "robot_video_summary.json")["pipeline_metrics"]
            require(metrics["diagnostic_recognition_delay_ms"] == 0 and metrics["capture_queue_limit"] == 8, "diagnostic or relaxed queue in matrix")
            item.update(adjudicate(session, case))
            item["passed"] = True
        except Exception as error:
            item["error"] = str(error)
        report["cases"].append(item)
        save(output / "report.json", report)
        print(json.dumps(item), flush=True)
        if not item["passed"] and args.stop_on_failure:
            break
    report["complete"] = len(report["cases"]) == 18
    report["passed"] = len(report["cases"]) == 18 and all(c["passed"] for c in report["cases"])
    save(output / "report.json", report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
