"""Read-only verification after the benchmark; write a new validation record."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=HERE / "final_validation.json")
    args = parser.parse_args()
    if (HERE.parent / ".benchmark.lock").exists():
        parser.error("Wait until all benchmarks finish before hashing artifacts")
    if args.out.exists():
        parser.error("Choose a new output path; existing validation is preserved")
    original = read(HERE / "current_validation.json")
    sources = read(HERE.parent / "20261019_parallel/source_files.json")
    changed = []
    for name, metadata in sources.items():
        path = ROOT / name
        if not path.is_file() or path.stat().st_size != metadata["size"] or sha(path) != metadata["sha256"]:
            changed.append(name)
    executables = []
    for metadata in original["executables"]:
        path = ROOT / "build/parallel/Release" / metadata["name"]
        digest = sha(path)
        executables.append({"name": metadata["name"], "sha256": digest,
                            "unchanged": digest == metadata["sha256"]})
    measured = read(HERE / "measured/runs.json")
    control = read(HERE / "control_no_parallel_roots/build_result.json")
    control_sha = sha(Path(control["executable"]["path"]))
    control_preflight = measured["preflight"]["binaries"]["control_no_roots4"]["sha256"]
    control_rows = [r for r in measured["runs"] if r["label"] == "control_no_roots4"]
    control_matches = (control_sha == control_preflight == control["executable"]["sha256"]
                       and len(control_rows) == 3
                       and all(r["exe_sha256"] == control_sha
                               and r["comparisons"]["baseline"]["equal"] is True
                               and r["comparisons"]["direct_numbers"]["equal"] is True
                               for r in control_rows))
    binaries_match = all(sha(Path(row["path"])) == row["sha256"]
                         for row in measured["preflight"]["binaries"].values())
    fixture_matches = sha(Path(measured["fixture"]["path"])) == measured["fixture"]["sha256"]
    driver_matches = sha(HERE / "run_stages.py") == measured["preflight"]["driver"]["sha256"]
    complete = measured["status"] == "complete" and not measured["failures"] and len(measured["runs"]) == 46
    passed = (not changed and all(row["unchanged"] for row in executables)
              and control_matches and binaries_match and fixture_matches and driver_matches and complete)
    result = {"recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": passed,
              "production_sources_verified": len(sources), "changed_production_source_paths": changed,
              "production_executables": executables,
              "all_measured_binaries_unchanged_after_series": binaries_match,
              "fixture_unchanged": fixture_matches, "driver_unchanged": driver_matches,
              "completed_processes": len(measured["runs"]), "series_complete_without_failures": complete,
              "control_build_exe_matches_measured_exe_and_three_numerical_comparisons": control_matches,
              "control_executable_sha256": control_sha,
              "control_checkpoint_build_id": control["checkpoint_build_id"],
              "control_numerical_evidence": [r["output"] for r in control_rows],
              "limits": "Checks original source inventory and saved production/benchmark executables. Does not claim every file on disk was inventoried. Numerical comparison evidence is from the completed benchmark; this verification does not rerun the solver or bypass checkpoint compatibility."}
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": passed, "source_files": len(sources), "production_executables": len(executables),
                      "measured_runs": len(measured["runs"]), "output": str(args.out)}, indent=2))
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
