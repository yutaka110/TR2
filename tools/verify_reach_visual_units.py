"""Build and run visual, world, and foundation tests without replacing old evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import csv


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--build-name", default="reach_g1_visual_20260921")
    parser.add_argument("--include-command", action="store_true")
    parser.add_argument("--include-link", action="store_true")
    parser.add_argument("--include-state", action="store_true")
    parser.add_argument("--include-baseline", action="store_true")
    parser.add_argument("--replay-history", action="store_true")
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("invalid name")
    repo = Path(__file__).resolve().parents[1]
    if not args.build_name.replace("_", "").replace("-", "").isalnum():
        parser.error("invalid build name")
    root = repo / "artifacts" / args.build_name / "verification" / args.name
    root.mkdir(parents=True, exist_ok=False)
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    msbuild = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild", "-find", "MSBuild/**/Bin/MSBuild.exe"], text=True).splitlines()[0]
    report = {"passed": False, "tests": []}
    try:
        visual_options = [str(root / "calibration.csv")]
        if args.replay_history:
            previous = repo / "artifacts/reach_g1_closed_loop_20260921/verification/matrix_03/report.json"
            fixture = root / "prior_observations.csv"
            with fixture.open("w", encoding="utf-8", newline="") as stream:
                writer = csv.writer(stream);writer.writerow(["case","capture_us","x","y","yaw","position_allowance","yaw_allowance","true_x","true_y","frame_id","original_valid"])
                for case in json.loads(previous.read_text(encoding="utf-8"))["cases"]:
                    if not case["passed"]:continue
                    session=Path(case["session"])
                    with (session / "capture_evaluation.csv").open() as source:
                        truth={r["frame_id"]:r for r in csv.DictReader(source)}
                    with (session / "observations.csv").open() as source:
                        for r in csv.DictReader(source):
                            if r["valid"] != "1":continue
                            t=truth[r["frame_id"]]
                            writer.writerow([case["id"],r["capture_us"],r["x_m"],r["y_m"],r["yaw_rad"],r["position_error_m"],r["yaw_error_rad"],t["x_m"],t["y_m"],r["frame_id"],r["valid"]])
            visual_options.append(str(fixture))
        cases = [
            ("visual", "reach_visual_tests", "reach_visual_tests", visual_options),
            ("world", "reach_robot_tests", "reach_robot_tests", []),
            ("foundation", "reach_g1_tests", "reach_g1_tests", [str(repo / "config/reach_rt_g1_foundation.json"), str(root / "foundation_sessions")]),
        ]
        if args.include_command:
            cases.insert(0, ("command", "reach_command_tests", "reach_command_tests", [str(root / "command_braking.csv")]))
        if args.include_link:
            cases.insert(0, ("link", "reach_link_tests", "reach_link_tests", [str(root / "link_unit_result.json")]))
        if args.include_state:
            cases.insert(0, ("state", "reach_state_tests", "reach_state_tests", []))
        if args.include_baseline:
            cases.insert(0, ("baseline", "reach_baseline_tests", "reach_baseline_tests", []))
        for name, project, executable, options in cases:
            folder = root / name
            folder.mkdir()
            command = [msbuild, str(repo / "tools" / (project + ".vcxproj")), "/nologo", "/nr:false", "/p:Configuration=Release", "/p:Platform=x64", "/v:minimal",
                       "/p:OutDir=" + str(folder / "bin") + os.sep, "/p:IntDir=" + str(folder / "obj") + os.sep]
            build = subprocess.run(command, cwd=repo, env=dict(os.environ), capture_output=True)
            (folder / "build.txt").write_bytes(build.stdout + build.stderr)
            if build.returncode:
                raise RuntimeError(name + " build failed")
            exe = folder / "bin" / (executable + ".exe")
            test = subprocess.run([str(exe), *options], cwd=repo, env=dict(os.environ), capture_output=True)
            (folder / "test.txt").write_bytes(test.stdout + test.stderr)
            item = {"name": name, "exit_code": test.returncode, "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                    "output": (test.stdout + test.stderr).decode("utf-8", errors="replace").strip()}
            report["tests"].append(item)
            print(item["output"], flush=True)
            if test.returncode:
                raise RuntimeError(name + " tests failed")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    report["source_hashes"] = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for folder in ("research", "tools") for p in (repo / folder).glob("*reach*" if folder == "tools" else "Reach*") if p.suffix in (".cpp", ".h", ".vcxproj")}
    if args.include_baseline:
        for relative in ("network/PacketPacer.cpp","network/PacketPacer.h","network/PacketProtocol.h","network/JitterBuffer.cpp","network/JitterBuffer.h","network/FrameReassembler.h"):
            report['source_hashes'][relative]=hashlib.sha256((repo/relative).read_bytes()).hexdigest()
    (root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
