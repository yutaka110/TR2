"""Summarize only the six prespecified existing-engine G0 runs."""
from collections import Counter
import csv
import json
from pathlib import Path
import statistics

root = Path(__file__).resolve().parents[1] / "artifacts/reach_g0_20260921"
records = []
for condition in ("baseline", "loss10"):
    for seed in (101, 202, 303):
        run = root / "runs" / f"{condition}_{seed}"
        manifest = json.loads((run / "manifest.json").read_text(encoding="utf-8"))
        if manifest["exit_code"] != 0 or not manifest["scenario_match"]:
            raise ValueError(f"Invalid run: {run}")
        files = list((run / "logs").glob("network_summary_*.csv"))
        if len(files) != 1:
            raise ValueError(f"Expected exactly one summary: {run}")
        with files[0].open(encoding="utf-8-sig", newline="") as f:
            rows = list(csv.DictReader(f))
        if len(rows) != 1:
            raise ValueError(f"Expected one scenario: {run}")
        row = rows[0]
        record = {"condition": condition, "seed": seed, "summary": str(files[0].relative_to(root)),
                  "executable_sha256": manifest["executable_sha256"]}
        for key in ("avgLatencyMs", "p95LatencyMs", "avgDisplayFps", "avgDecodeFps",
                    "avgPacketLossRate", "measurementSec", "displayedFrames", "simDroppedPackets",
                    "deadlineNackSentFrames", "deadlineNackRecoveredFrames", "fecRecoveredFrames"):
            record[key] = float(row[key])
        record["legacy_verdict"] = row["verdict"]
        # This sampled trace proves that decoding occurred, not that its frame identity was correct.
        rx_files = list((run / "logs").glob("h264_receive_trace_*.csv"))
        with rx_files[0].open(encoding="utf-8-sig", newline="") as f:
            record["sampled_decode_status_counts"] = dict(Counter(r["decodeStatus"] for r in csv.DictReader(f)))
        records.append(record)
if len({r["executable_sha256"] for r in records}) != 1:
    raise ValueError("Baseline runs used different executables")
groups = {}
for condition in ("baseline", "loss10"):
    subset = [r for r in records if r["condition"] == condition]
    groups[condition] = {key: {"mean_of_runs": statistics.mean(r[key] for r in subset),
                              "min": min(r[key] for r in subset), "max": max(r[key] for r in subset)}
                         for key in ("avgDisplayFps", "avgLatencyMs", "p95LatencyMs", "measurementSec")}
result = {
    "purpose": "G0 engine smoke/performance reference, not a research efficacy test",
    "limitations": ["n=3 per condition; no efficacy inference", "legacy latency uses unverified frame identity",
                    "latency p95 is inherited from sampled engine telemetry, not raw per-frame end-to-end data",
                    "20s requested; 5s warmup; measurement span is taken from logs", "hardware/OS scheduling is not deterministic"],
    "runs": records, "groups": groups,
}
(root / "baseline_summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(groups, indent=2))
