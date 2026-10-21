"""Alternating old/new CLI runs with the same 25 us configuration."""
import argparse
import hashlib
import json
import re
import statistics
import subprocess
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    before = sha(args.config)
    assert json.loads(args.config.read_text(encoding="utf-8"))["dt"] == 25e-6
    binaries = {"old": args.baseline.resolve(), "new": args.current.resolve()}
    runs = []
    started = time.perf_counter()
    for index, name in enumerate(("old", "new", "new", "old", "old", "new")):
        prefix = args.output / f"{index}_{name}"
        command = [str(binaries[name]), "--config", str(args.config), "--threads", "8",
                   "--stop-at", ".1", "--output", str(prefix)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
        prefix.with_suffix(".log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        elapsed = float(re.search(r"^Wall seconds: ([^\n]+)", result.stdout, re.M)[1])
        runs.append({"build": name, "solve_seconds": elapsed, "command": command})
        for suffix in ("trace", "events", "deliveries", "weights", "stimuli", "shared_stimuli"):
            assert Path(str(prefix) + "_" + suffix + ".csv").read_bytes() == (
                args.output / ("0_old_" + suffix + ".csv")).read_bytes()
        print(f"{name}: {elapsed:.6f} s", flush=True)
    aggregates = {}
    for name in binaries:
        values = [run["solve_seconds"] for run in runs if run["build"] == name]
        aggregates[name] = {"median_s": statistics.median(values), "minimum_s": min(values),
                            "maximum_s": max(values), "sha256": sha(binaries[name])}
    assert sha(args.config) == before
    report = {"input_sha256": before, "runs": runs, "aggregate": aggregates,
              "new_over_old_ratio": aggregates["new"]["median_s"] / aggregates["old"]["median_s"],
              "elapsed_seconds": time.perf_counter() - started, "csv_byte_exact": True,
              "model_seconds_per_run": .1}
    (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
