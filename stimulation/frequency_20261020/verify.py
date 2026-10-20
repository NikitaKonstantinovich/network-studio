"""Small process/compatibility check; never a 150-second laboratory run."""
import argparse
import filecmp
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--previous-cli", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
args.out = args.out.resolve()
args.out.mkdir(parents=True, exist_ok=False)
started = time.perf_counter()
cli = root / "build/Release/network_studio_cli.exe"
test = root / "build/Release/network_studio_stimulator_timing_tests.exe"
report = {"model_duration_seconds": 0.003, "commands": []}


def run(name, command, success=True):
    result = subprocess.run([str(v) for v in command], cwd=root, capture_output=True)
    (args.out / (name + ".log")).write_bytes(result.stdout + result.stderr)
    report["commands"].append({"command": [str(v) for v in command], "exit_code": result.returncode})
    if success and result.returncode:
        raise RuntimeError(name + " failed")
    return result


run("probe", [test, "--probe"])
run("create", [test, "--create", args.out / "resume"])
run("resume", [test, "--resume", args.out / "resume"])
report["resume_checkpoint_identical"] = filecmp.cmp(args.out / "resume/continuous.nscp", args.out / "resume/resumed.nscp", shallow=False)
assert report["resume_checkpoint_identical"]
report["resume_model_duration_seconds"] = 0.36
if args.previous_cli:
    old = args.previous_cli.resolve()
    for name, exe in [("old", old), ("new", cli)]:
        run(name, [exe, "--preset", "6", "--duration", ".003", "--checkpoint", args.out / (name + ".nscp"), "--output", args.out / name])
    result = run("incompatible", [cli, "--resume", args.out / "old.nscp", "--output", args.out / "rejected"], success=False)
    report["old_checkpoint_rejected"] = result.returncode != 0 and b"incompatible solver build" in result.stdout + result.stderr
    assert report["old_checkpoint_rejected"]
    files = sorted(args.out.glob("old_*.csv"))
    assert len(files) >= 10
    report["unchanged_csv"] = []
    for path in files:
        other = args.out / ("new_" + path.name[4:])
        assert filecmp.cmp(path, other, shallow=False), path.name
        report["unchanged_csv"].append(path.name[4:])
report["wall_seconds"] = time.perf_counter() - started
report["sha256"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                    [cli, root / "build/Release/network_studio.exe", test]}
(args.out / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
