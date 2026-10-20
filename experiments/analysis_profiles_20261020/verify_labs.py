"""Read-only reanalysis of existing 400 s lab exports. Does not run a simulation."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--labs", type=Path, default=root.parent / "labs")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
out = args.output or root / "verification" / ("analysis_profiles_" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
out = out.resolve()
if out == args.labs.resolve() or args.labs.resolve() in out.parents:
    raise SystemExit("Use a new output directory outside labs")
out.mkdir(parents=True, exist_ok=False)
cli = root / "build" / "Release" / "network_studio_cli.exe"
profile = Path(__file__).with_name("lab6_profile.json")
paths = [args.labs / f"lab6_{hz}Hz{suffix}" for hz in (1, 10) for suffix in
         ("_experiment.json", "_summary.json", "_events.csv", "_shared_stimuli.csv")]
paths += [args.labs / "lab6_comparison" / "common_window_statistics.json"]
def digest(p):
    with p.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()
before = {str(p): digest(p) for p in paths}
durations = {}
def run(name, arguments, expected=0):
    start = time.perf_counter()
    result = subprocess.run([str(cli), *map(str, arguments)], capture_output=True)
    durations[name] = time.perf_counter() - start
    (out / (name + ".log")).write_bytes(result.stdout + result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"{name}: {result.returncode}; see log")
for hz in (1, 10):
    run(f"lab6_{hz}Hz", ["--analyze-export", args.labs / f"lab6_{hz}Hz", "--analysis-profile", profile,
                        "--analysis-from", 280, "--analysis-to", 400, "--output", out / f"lab6_{hz}Hz"])
run("compare", ["--compare-analysis", out / "lab6_1Hz_analysis.json", out / "lab6_10Hz_analysis.json"])
run("partial", ["--analyze-export", args.labs / "lab6_10Hz", "--analysis-profile", profile,
                "--analysis-from", 250, "--analysis-to", 400, "--output", out / "partial"])
run("reject_partial", ["--compare-analysis", out / "partial_analysis.json", out / "partial_analysis.json"], 2)
data = {str(hz): json.loads((out / f"lab6_{hz}Hz_analysis.json").read_text(encoding="utf-8")) for hz in (1, 10)}
previous = json.loads(paths[-1].read_text(encoding="utf-8"))
for hz in ("1", "10"):
    for key in ("detected_bursts", "complete_bursts", "eligible_stimuli", "matched", "missed", "response_fraction"):
        assert data[hz][key] == previous[hz][key], (hz, key, data[hz][key], previous[hz][key])
    assert data[hz]["full_interval"] and data[hz]["prehistory_complete"]
after = {str(p): digest(p) for p in paths}
assert before == after, "Input lab files changed"
manifest = {"description": __doc__, "input_sha256": before, "inputs_unchanged": True,
            "cli_sha256": digest(cli), "profile_sha256": digest(profile), "wall_seconds": durations,
            "interval_s": [280, 400], "simulation_steps_executed": 0, "results": data}
(out / "verification.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(out)
print(json.dumps(durations, indent=2))
