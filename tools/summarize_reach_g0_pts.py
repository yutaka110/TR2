"""Audit sampled decoder timestamps, without assuming encoder/capture identity."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    paths = list((args.run / "logs").glob("h264_receive_trace_*.csv"))
    if len(paths) != 1:
        raise ValueError("Expected one receive trace")
    manifest = json.loads((args.run / "manifest.json").read_text(encoding="utf-8"))
    if manifest["exit_code"] != 0 or not manifest["scenario_match"]:
        raise ValueError("Run did not complete with the requested scenario")
    with paths[0].open(encoding="utf-8-sig", newline="") as file:
        decoded = [row for row in csv.DictReader(file) if row["decodeStatus"] == "Decoded"]
    valid = [row for row in decoded if row["outputPtsValid"] == "1"]
    if not valid:
        raise ValueError("No valid decoder output timestamps")
    differences = [int(row["inputPtsUs"]) * 10 - int(row["outputPts100ns"]) for row in valid]
    result = {
        "executable_sha256": manifest["executable_sha256"],
        "trace": paths[0].name, "sampled_decoded_rows": len(decoded),
        "valid_output_pts_rows": len(valid),
        "mismatch_rows": sum(value != 0 for value in differences),
        "input_minus_output_pts_100ns_counts": dict(Counter(differences)),
        "limitations": ["Sampled trace, not every frame",
                        "Does not certify encoder output to camera capture identity",
                        "G0 adds observation only; attribution is not fixed"],
    }
    (args.run / "pts_audit_summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
