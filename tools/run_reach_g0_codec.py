"""Build and run the G0 codec probe in new, isolated output directories."""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True, help="Unique run group, e.g. codec_recheck_01")
    args = parser.parse_args()
    if not args.name.replace("_", "").replace("-", "").isalnum():
        parser.error("name must contain only letters, digits, underscore or hyphen")
    repo = Path(__file__).resolve().parents[1]
    root = repo / "artifacts/reach_g0_20260921"
    group = root / args.name
    group.mkdir(parents=True, exist_ok=False)
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    msbuild = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                      "Microsoft.Component.MSBuild", "-find", "MSBuild/**/Bin/MSBuild.exe"],
                                     text=True).splitlines()[0]
    command = [msbuild, "tools/reach_g0_codec_probe.vcxproj", "/nologo", "/m:1", "/nr:false",
               "/p:Configuration=Release", "/p:Platform=x64", "/v:minimal",
               f"/flp:logfile={group / 'build.log'};verbosity=normal;encoding=UTF-8"]
    (group / "build_command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    subprocess.run(command, cwd=repo, env=dict(os.environ), check=True)
    exe = root / "probe_bin/reach_g0_codec_probe.exe"
    inputs = [exe, repo / "tools/reach_g0_codec_probe.cpp", repo / "network/H264Encoder.cpp",
              repo / "network/H264Encoder.h", repo / "tools/inspect_reach_g0_codec.py"]
    hashes = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    for mode in ("auto", "software"):
        out = group / mode
        out.mkdir()
        # Do not inherit unrelated encoder experiments from the parent session.
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith("RNVP_")}
        start = datetime.now().astimezone().isoformat()
        result = subprocess.run([str(exe), mode], cwd=out, env=env, capture_output=True, timeout=30)
        (out / "stdout.txt").write_bytes(result.stdout)
        (out / "stderr.txt").write_bytes(result.stderr)
        manifest = {"started_at": start, "mode": mode, "exit_code": result.returncode,
                    "command": [str(exe), mode], "sha256": hashes}
        (out / "run.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        result.check_returncode()
        subprocess.run([sys.executable, str(repo / "tools/inspect_reach_g0_codec.py"), str(out)], check=True)


if __name__ == "__main__":
    main()
