"""Sequential benchmark runner; never overlaps repetitions or GUI/CLI runs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(reference, actual, require_dump=False):
    # Checkpoints intentionally include strict build compatibility metadata.
    files = sorted(p.name for p in reference.glob("result_*.csv"))
    if files != sorted(p.name for p in actual.glob("result_*.csv")):
        raise RuntimeError("Numerical CSV file sets differ")
    if require_dump and not all((p / "numbers.bin").is_file() for p in (reference, actual)):
        raise RuntimeError("CLI comparison requires complete numbers.bin dumps on both sides")
    if (reference / "numbers.bin").exists() and (actual / "numbers.bin").exists():
        files.append("numbers.bin")
    if not files:
        raise RuntimeError(f"No numerical reference files in {reference}")
    checks = {name: {"reference": sha(reference / name), "actual": sha(actual / name)} for name in files}
    mismatches = [name for name, x in checks.items() if x["reference"] != x["actual"]]
    report = {"byte_equal": not mismatches, "mismatches": mismatches, "files": checks,
              "checkpoint_comparison": "Excluded envelope; compatibility checks are unchanged. numbers.bin includes complete final state and all STDP traces/weights."}
    (actual / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if mismatches:
        raise RuntimeError("Numerical difference: " + ", ".join(mismatches))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", required=True, type=Path)
    p.add_argument("--fixture", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--reference", type=Path)
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--gui", action="store_true")
    a = p.parse_args()
    if a.repeat < 1:
        p.error("repeat must be positive")
    a.exe = a.exe.resolve(); a.fixture = a.fixture.resolve(); a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    lock = Path(__file__).resolve().parent / ".benchmark.lock"
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        os.write(fd, str(os.getpid()).encode()); os.close(fd)
        rows = []
        reference = a.reference.resolve() if a.reference else None
        for i in range(a.repeat):
            out = a.out / f"run_{i+1}"
            out.mkdir()  # Never silently overwrite an existing measurement.
            command = [str(a.exe), str(a.fixture)]
            command += ["--benchmark", str(out)] if a.gui else [str(out)]
            start = time.perf_counter()
            with (out / "process.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            elapsed = time.perf_counter() - start
            if result.returncode:
                raise RuntimeError(f"Exit {result.returncode}; see {out}")
            timing = json.loads((out / ("benchmark.json" if a.gui else "timing.json")).read_text(encoding="utf-8"))
            timing.update(process_seconds=elapsed, run=i+1)
            compared = reference is not None
            if compared:
                compare(reference, out, require_dump=not a.gui)
            else:
                reference = out
            rows.append(timing)
            report = {"executable": str(a.exe), "exe_sha256": sha(a.exe), "fixture": str(a.fixture),
                      "fixture_sha256": sha(a.fixture), "gui": a.gui, "runs": rows}
            key = "wall_seconds" if a.gui else "solve_seconds"
            if key in rows[0]:
                values = [r[key] for r in rows]
                report["summary"] = {"metric": key, "median": statistics.median(values), "min": min(values),
                                     "max": max(values), "stdev": statistics.stdev(values) if len(values) > 1 else None}
            (a.out / "runs.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            status = "numerical comparison OK" if compared else "reference captured"
            print(f"Finished {out.name}: {elapsed:.3f}s process, {status}", flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
