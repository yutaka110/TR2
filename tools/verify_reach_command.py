"""Independently decode RCMD wire logs and audit robot-side actuation/braking."""
import argparse
import bisect
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import zlib


def require(ok, why):
    if not ok:
        raise AssertionError(why)


def wire(text):
    data = bytes.fromhex(text)
    require(len(data) == 88, "wire size")
    require(data[:8] == b"RCMD\0\1\0X", "wire magic/version/size")
    require(zlib.crc32(data[:84]) == int.from_bytes(data[84:], "big"), "wire CRC")
    require(data[80:84] == bytes(4), "wire reserved")
    seq, generated, deadline, capture, frame, stream, v, w, state, reason, flags = struct.unpack("!QQQQIIiiHHI", data[24:80])
    return dict(session=data[8:24].hex(), sequence=seq, generated=generated, deadline=deadline, capture=capture,
                frame=frame, stream=stream, v=v / 1e6, w=w / 1e6, state=state, reason=reason, flags=flags)


def audit(session, link_loss=False):
    read = lambda n: json.loads((session / n).read_text(encoding="utf-8"))
    rows = lambda n: list(csv.DictReader((session / n).open(encoding="utf-8", newline="")))
    summary, model, video = read("command_udp_summary.json"), read("command_model.json"), read("robot_video_summary.json")
    require(video["validation_passed"] and video["identity_errors"] == 0, "video integrity")
    require(read("manifest.json")["capabilities"]["command_udp"], "manifest capability")
    commands = {int(r["sequence"]): r for r in rows("commands.csv")}
    observations = {int(r["frame_id"]): r for r in rows("observations.csv")}
    tx, rx, applied, world = rows("command_tx.csv"), rows("command_rx.csv"), rows("udp_applied_commands.csv"), rows("world.csv")
    require(sum(int(r["ip_bytes"]) for r in tx) == summary["attempted_ip_bytes"], "IP byte accounting")
    require(sum(r["event"] == "sent" for r in tx) == summary["sent_datagrams"], "send count")
    require(len(rx) == summary["received_datagrams"], "receive count")
    sent = [r["wire_hex"] for r in tx if r["event"] == "sent"]
    from collections import Counter
    if link_loss:
        require(model['capacity_queue_model'], 'capacity audit without link model')
        link=rows('downlink_link.csv');arrivals={r['packet_id']:r for r in link if r['event'] in ('admitted','tail_drop') and r['wire_hex'].startswith('52434d44')}
        require(Counter(sent)==Counter(r['wire_hex'] for r in arrivals.values()),'command ingress lost before modeled queue')
        delivered=[arrivals[r['packet_id']]['wire_hex'] for r in link if r['event']=='delivered' and r['packet_id'] in arrivals]
        require(Counter(delivered)==Counter(r['wire_hex'] for r in rx),'unexpected UDP loss after modeled link')
    else:
        require(summary["received_datagrams"] == summary["sent_datagrams"], "unexpected actual UDP loss")
        require(Counter(sent) == Counter(r["wire_hex"] for r in rx), "received bytes differ from UDP sends")
    accepted, accepted_times = [], []
    highest = 0
    last = None
    for packet in rx:
        status, now = packet["status"], int(packet["received_us"])
        if status in ("invalid_crc", "invalid_length"):
            require(int(packet["accepted_sequence"]) == (last["sequence"] if last else 0), "malformed changed accepted state")
            continue
        p = wire(packet["wire_hex"])
        require(p["deadline"] - p["generated"] == 100000, "wire lifetime")
        if status == "accepted":
            require(p["session"] == uuid.UUID(session.name).hex, "session mismatch accepted")
            require(p["sequence"] > highest and p["generated"] <= now < p["deadline"], "invalid acceptance order/deadline")
            source = commands[p["sequence"]]
            require(p["generated"] == int(source["generated_us"]) and p["capture"] == int(source["source_capture_us"]), "wire timestamps changed")
            require(p["frame"] == int(source["source_frame_id"]) and p["stream"] == int(source["source_stream_id"]), "wire image provenance changed")
            require(abs(p["v"] - float(source["v_m_s"])) <= .50001e-6 and abs(p["w"] - float(source["w_rad_s"])) <= .50001e-6, "fixed point quantisation")
            if p["v"] or p["w"]:
                obs = observations[p["frame"]]
                require(obs["valid"] == "1" and int(obs["received_us"]) <= p["generated"], "movement lacks prior image estimate")
            p["accepted"] = now
            last = p
            accepted.append(p)
            accepted_times.append(now)
        elif status == "expired_on_arrival":
            require(now >= p["deadline"], "arrival classified expired too early")
        elif status == "duplicate":
            require(p["sequence"] == highest, "duplicate classification")
        elif status == "reordered":
            require(p["sequence"] < highest, "reorder classification")
        elif status == "wrong_session":
            require(p["session"] != uuid.UUID(session.name).hex, "foreign session diagnostic")
        elif status == "future_command":
            require(p["generated"] > now, "future diagnostic")
        else:
            raise AssertionError("unexpected receive result " + status)
        if status != "accepted":
            require(int(packet["last_accepted_us"]) == (last["accepted"] if last else 0), "rejected packet renewed watchdog")
        highest = int(packet["highest_sequence"])
    require(len(applied) == len(world), "physics application count")
    previous_v = previous_w = 0.
    for application, state in zip(applied, world):
        now = int(application["applied_us"])
        index = bisect.bisect_right(accepted_times, now) - 1
        p = accepted[index] if index >= 0 else None
        heartbeat = p["accepted"] if p else model["origin_us"]
        reason = "watchdog_timeout" if now - heartbeat >= 250000 else "awaiting_command" if not p else "command_expired" if now >= p["deadline"] else "active"
        require(application["reason"] == reason, "robot uses wrong deadline/watchdog state")
        require(int(application["sequence"]) == (p["sequence"] if p else 0), "robot bypassed UDP accepted state")
        v, w = (p["v"], p["w"]) if reason == "active" else (0., 0.)
        require(abs(float(application["v_m_s"]) - v) < 1e-10 and abs(float(application["w_rad_s"]) - w) < 1e-10, "actuator target differs from received command")
        if state["collision"] == state["out_of_bounds"] == state["success"] == state["timeout"] == "0":
            expected_v = previous_v + max(-(.006 if v < previous_v else .003), min((.006 if v < previous_v else .003), v - previous_v))
            expected_w = previous_w + max(-.016, min(.016, w - previous_w))
            require(abs(float(state["v_m_s"]) - expected_v) < 1e-9, "incorrect linear acceleration/braking")
            require(abs(float(state["w_rad_s"]) - expected_w) < 1e-9, "incorrect angular braking")
        previous_v, previous_w = float(state["v_m_s"]), float(state["w_rad_s"])
    scenario = summary["scenario"]
    status = summary["statuses"]
    if scenario == "duplicate":
        require(status.get("duplicate", 0) > 0 and status["accepted"] == len(commands), "duplicate test did not occur")
    if scenario == "reorder":
        require(status.get("reordered", 0) == 1 and not any(p["sequence"] == 20 for p in accepted), "old command overrode newer command")
    if scenario == "late":
        require(status.get("expired_on_arrival", 0) > 0 and not accepted and max(float(w["v_m_s"]) for w in world) == 0, "delayed commands moved robot")
        require(any(a["reason"] == "watchdog_timeout" for a in applied), "late packets suppress watchdog")
    if scenario == "invalid":
        require(all(status.get(s, 0) == 1 for s in ("wrong_session", "invalid_crc", "invalid_length", "future_command")), "invalid datagram cases missing")
        require(accepted[-1]["sequence"] == len(commands), "invalid high sequence poisoned receiver")
    braking = None
    if scenario in ("outage", "recovery"):
        last_good = next(p for p in accepted if p["sequence"] == 40)
        start = next(i for i, a in enumerate(applied) if a["reason"] == "command_expired" and int(a["sequence"]) == 40)
        stop = next(i for i in range(start, len(world)) if float(world[i]["v_m_s"]) == 0 and float(world[i]["w_rad_s"]) == 0)
        v0 = float(world[start - 1]["v_m_s"])
        distance = sum((float(world[i - 1]["v_m_s"]) + float(world[i]["v_m_s"])) * .005 for i in range(start, stop + 1))
        require(v0 > .25 and 0 < float(world[start]["v_m_s"]) < v0, "outage did not brake moving robot gradually")
        require(abs(distance - v0 * v0 / 1.2) < .00004, "braking path differs from v^2/(2a)")
        require(stop - start + 1 == math.ceil((v0 - 1e-9) / .006), "wrong braking duration")
        require(any(a["reason"] == "watchdog_timeout" for a in applied[start:stop + 1]), "watchdog not observed during braking")
        braking = {"initial_v_m_s": v0, "path_m": distance, "physics_duration_s": (stop-start+1)/100,
                   "deadline_detection_lag_us": int(applied[start]["applied_us"]) - last_good["deadline"],
                   "stop_wall_us_after_last_accept": int(applied[stop]["applied_us"]) - last_good["accepted"]}
        if scenario == "outage":
            require(not any(int(a["sequence"]) > 40 for a in applied) and all(float(w["v_m_s"]) == 0 for w in world[stop:]), "movement revived during outage")
        else:
            require(any(int(a["sequence"]) >= 81 and float(a["v_m_s"]) > 0 for a in applied[stop:]), "fresh commands did not recover")
    return {"scenario": scenario, "session": str(session), "summary": summary, "decoded_images": video["decoded"],
            "physics_steps_audited": len(world), "local_evaluator_success": read("visual_control_summary.json")["local_evaluator_success"], "braking": braking}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--build-name", default="reach_g1_command_20260921")
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("invalid name")
    repo = Path(__file__).resolve().parents[1]
    if not args.build_name.replace("_", "").replace("-", "").isalnum():
        parser.error("invalid build name")
    root = repo / "artifacts" / args.build_name
    output = root / "verification" / args.name
    output.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "executable_sha256": hashlib.sha256((root / "bin/Release/GE3.exe").read_bytes()).hexdigest(), "cases": []}
    try:
        for scenario, duration in [("normal",12),("duplicate",3),("reorder",3),("late",4),("outage",4),("recovery",6),("invalid",3)]:
            name = args.name + "_" + scenario
            result = subprocess.run([sys.executable, str(repo / "tools/run_reach_g1.py"), "--stage", "command_udp", "--name", name,
                                     "--build-name", args.build_name, "--command-scenario", scenario, "--duration", str(duration)], cwd=repo, capture_output=True)
            (output / (scenario + ".stdout.txt")).write_bytes(result.stdout)
            (output / (scenario + ".stderr.txt")).write_bytes(result.stderr)
            require(result.returncode == 0, scenario + " app failed")
            invocation = root / "runs" / name
            launch = json.loads((invocation / "launch.json").read_text(encoding="utf-8"))
            require(launch["executable_sha256"] == report["executable_sha256"], "binary changed")
            item = audit(next((invocation / "sessions").iterdir()))
            report["cases"].append(item)
            print(json.dumps({"scenario": scenario, "statuses": item["summary"]["statuses"], "braking": item["braking"]}), flush=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
