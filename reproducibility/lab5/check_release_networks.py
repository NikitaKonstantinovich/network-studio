"""Compare 12 published-release network runs with frozen lab5 CSV outputs.

This check neither changes production files nor regenerates baseline results.
New optional JSON fields may be serialized with their inactive default values;
the trace, events, and weights CSV files must remain byte-identical.

Prepare (read-only): python check_release_networks.py --prepare-only
Run after publishing: python check_release_networks.py --expected-cli-sha256 HASH
"""
from __future__ import annotations
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[1]
OUTPUT = HERE / "qa/release_networks"
REPORT = HERE / "release_network_check.json"
CSV_SUFFIXES = ("trace.csv", "events.csv", "weights.csv")

def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))

def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)

def output_file(prefix, suffix):
    return prefix.with_name(prefix.name + "_" + suffix)

def strip_new_inactive_defaults(config):
    normalized = copy.deepcopy(config)
    for n in normalized["neurons"]:
        if "pulse_intervals" in n:
            if n["pulse_intervals"] != []:
                raise AssertionError("Export introduced nonempty stimulus intervals")
            del n["pulse_intervals"]
    for s in normalized["synapses"]:
        if "tau_slow" in s:
            if s["tau_slow"] != .1:
                raise AssertionError("Export changed the inactive default slow time constant")
            del s["tau_slow"]
    return normalized

def prepare():
    paths_file = HERE / "paths/findings.json"
    duration_file = HERE / "duration/findings.json"
    paths = read(paths_file)
    duration = read(duration_file)
    old_sha = paths["provenance"]["cli_sha256"]
    if duration["cli_sha256"] != old_sha:
        raise AssertionError("Baseline series were produced by different binaries")
    entries = []
    for topology in ("two", "chain", "shortcut"):
        for condition in ("seed00_noise3_dt25us", "seed00_noise3_dt12.5us", "seed00_noise0_dt25us"):
            name = topology + "_" + condition
            row = paths["runs"][name]
            entries.append(("paths", name, row["config_sha256"], row["metrics"]["hashes"]))
    for topology, seconds in (("two", "6.1"), ("chain", "12.525"), ("shortcut", "12.525")):
        name = topology + "_seed00_duration" + seconds
        row = next(r for r in duration["rows"] if r["case"] == name)
        entries.append(("duration", name, row["config_sha256"], row["raw_sha256"]))
    cases = []
    for series, name, config_hash, hashes in entries:
        config_path = HERE / series / "configs" / (name + ".json")
        baseline_prefix = HERE / series / "raw" / name
        if sha(config_path) != config_hash:
            raise AssertionError(f"Frozen input hash mismatch: {config_path}")
        config = read(config_path)
        if config["schema"] != 4:
            raise AssertionError("Expected the exact saved schema4 input")
        # Verify frozen raw provenance before running anything new.
        for suffix in CSV_SUFFIXES + ("summary.json", "experiment.json"):
            p = output_file(baseline_prefix, suffix)
            if sha(p) != hashes[p.name]:
                raise AssertionError(f"Frozen output hash mismatch: {p}")
        summary = read(output_file(baseline_prefix, "summary.json"))
        if not summary["complete"] or summary["error"] or summary["time_s"] != config["duration"]:
            raise AssertionError(f"Incomplete baseline: {name}")
        if strip_new_inactive_defaults(read(output_file(baseline_prefix, "experiment.json"))) != strip_new_inactive_defaults(config):
            raise AssertionError(f"Baseline snapshot differs from saved input: {name}")
        cases.append(dict(series=series, case=name, config_path=config_path, config_sha256=config_hash,
                          config=config, baseline_prefix=baseline_prefix, baseline_hashes=hashes))
    return cases, dict(cli_sha256=old_sha, findings_sha256={
        "paths/findings.json": sha(paths_file), "duration/findings.json": sha(duration_file)})

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=STUDIO / "build/Release/network_studio_cli.exe")
    parser.add_argument("--expected-cli-sha256", help="Required for execution, pins the newly published binary")
    parser.add_argument("--prepare-only", action="store_true", help="Verify all baseline files without running CLI or writing output")
    args = parser.parse_args()
    cases, baseline = prepare()
    if args.prepare_only:
        print(json.dumps(dict(prepared=True, cases=len(cases), csv_files=len(cases)*3, baseline=baseline), indent=2))
        return 0
    if not args.expected_cli_sha256:
        parser.error("Execution requires --expected-cli-sha256 from the published build")
    cli = args.cli.resolve()
    cli_sha = sha(cli)
    if cli_sha != args.expected_cli_sha256.lower():
        raise AssertionError("Published CLI hash differs from the pinned value")
    if cli_sha == baseline["cli_sha256"]:
        raise AssertionError("The CLI is still the frozen pre-triplet build")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    result = dict(scope=__doc__, checked_at_utc=datetime.now(timezone.utc).isoformat(),
                  cli=cli.as_posix(), cli_sha256=cli_sha, script_sha256=sha(Path(__file__)),
                  python=sys.version, baseline=baseline, expected_run_count=12,
                  output_directory=OUTPUT.as_posix(), runs=[])
    for case in cases:
        name = case["case"]
        prefix = OUTPUT / (case["series"] + "__" + name)
        command = [str(cli), "--config", str(case["config_path"]), "--output", str(prefix)]
        tick = time.perf_counter()
        row = dict(case=name, series=case["series"], config=case["config_path"].as_posix(),
                   config_sha256=case["config_sha256"], output_prefix=prefix.as_posix(), command=command,
                   passed=False, complete=False)
        try:
            run = subprocess.run(command, capture_output=True, text=True, check=False, timeout=120)
            output_file(prefix, "stdout.txt").write_text(run.stdout + run.stderr, encoding="utf-8")
            row["exit_code"] = run.returncode
            if run.returncode:
                raise AssertionError(f"CLI exited with {run.returncode}: {run.stdout[-500:]} {run.stderr[-500:]}")
            summary = read(output_file(prefix, "summary.json"))
            row["complete"] = summary["complete"] and not summary["error"] and summary["time_s"] == case["config"]["duration"]
            row["time_s"] = summary["time_s"]
            row["steps"] = summary["steps"]
            row["neuron_spikes"] = summary["neuron_spikes"]
            row["final_synapse_weights"] = summary["final_synapse_weights"]
            row["summary_semantically_equal"] = summary == read(output_file(case["baseline_prefix"], "summary.json"))
            row["config_snapshot_equal_ignoring_new_inactive_defaults"] = (
                strip_new_inactive_defaults(read(output_file(prefix, "experiment.json")))) == strip_new_inactive_defaults(case["config"])
            row["csv"] = {}
            for suffix in CSV_SUFFIXES:
                old = output_file(case["baseline_prefix"], suffix)
                new = output_file(prefix, suffix)
                old_hash, new_hash = sha(old), sha(new)
                row["csv"][suffix] = dict(baseline_path=old.as_posix(), baseline_sha256=old_hash,
                    release_path=new.as_posix(), release_sha256=new_hash, baseline_bytes=old.stat().st_size,
                    release_bytes=new.stat().st_size, byte_identical=old_hash == new_hash,
                    frozen_hash_unchanged=old_hash == case["baseline_hashes"][old.name])
            row["input_hash_unchanged"] = sha(case["config_path"]) == case["config_sha256"]
            row["passed"] = (row["complete"] and row["summary_semantically_equal"] and
                row["config_snapshot_equal_ignoring_new_inactive_defaults"] and row["input_hash_unchanged"] and
                all(v["byte_identical"] and v["frozen_hash_unchanged"] for v in row["csv"].values()))
        except Exception as exc:
            row["error"] = f"{type(exc).__name__}: {exc}"
        row["wall_seconds"] = time.perf_counter() - tick
        result["runs"].append(row)
        print(f"{name}: {'PASS' if row['passed'] else 'FAIL'} ({row['wall_seconds']:.3f} s)", flush=True)
        write_json(REPORT, result)
    result["cli_sha256_after"] = sha(cli)
    result["baseline_findings_unchanged"] = all(sha(HERE / p) == h for p,h in baseline["findings_sha256"].items())
    result["run_count"] = len(result["runs"])
    result["byte_identical_csv_count"] = sum(v["byte_identical"] for r in result["runs"] for v in r.get("csv", {}).values())
    result["wall_seconds"] = time.perf_counter() - started
    result["passed"] = (len(result["runs"]) == 12 and all(r["passed"] for r in result["runs"]) and
                         result["cli_sha256_after"] == cli_sha and result["baseline_findings_unchanged"])
    write_json(REPORT, result)
    print(f"Release network regression: {'PASS' if result['passed'] else 'FAIL'}, "
          f"{result['byte_identical_csv_count']}/36 identical CSVs; {result['wall_seconds']:.3f} s", flush=True)
    return 0 if result["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
