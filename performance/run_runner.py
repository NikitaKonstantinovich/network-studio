"""Run one sequential benchmark series and compare authoritative exports/checkpoints."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

import compare_checkpoint


CHUNK = 1024 * 1024


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def equal_files(left, right):
    if left.stat().st_size != right.stat().st_size:
        return False
    with left.open("rb") as a, right.open("rb") as b:
        while True:
            x, y = a.read(CHUNK), b.read(CHUNK)
            if x != y:
                return False
            if not x:
                return True


def compare(reference, actual, checkpoint_mode):
    files = sorted(p.name for p in reference.glob("result_*.csv"))
    actual_files = sorted(p.name for p in actual.glob("result_*.csv"))
    if not files or files != actual_files:
        raise RuntimeError("Numerical CSV file sets are empty or differ")
    checks = {name: {"byte_equal": equal_files(reference / name, actual / name),
                     "reference_sha256": sha(reference / name), "actual_sha256": sha(actual / name)}
              for name in files}
    before, after = reference / "final.nscp", actual / "final.nscp"
    if not before.is_file() or not after.is_file():
        raise RuntimeError("Both reference and actual must contain final.nscp")
    if checkpoint_mode == "exact":
        checkpoint = {"checkpoint_files_byte_equal": equal_files(before, after),
                      "reference_sha256": sha(before), "actual_sha256": sha(after),
                      "excluded_fields": [],
                      "guarantee": "All serialized checkpoint bytes, including build identity and checksum, are compared. No checkpoint is loaded, modified, or resumed."}
        checkpoint_equal = checkpoint["checkpoint_files_byte_equal"]
    else:
        checkpoint = compare_checkpoint.compare(before, after)
        checkpoint_equal = checkpoint["equal_except_build_metadata"]
    report = {"equal": all(row["byte_equal"] for row in checks.values()) and checkpoint_equal,
              "reference": str(reference), "actual": str(actual), "csv": checks,
              "checkpoint_mode": checkpoint_mode, "checkpoint": checkpoint}
    (actual / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not report["equal"]:
        raise RuntimeError(f"Numerical/checkpoint comparison failed; see {actual / 'comparison.json'}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mode", choices=("runner", "cli", "gui"), default="runner",
                        help="cli means the direct solver benchmark executable, not the general CLI")
    parser.add_argument("--reference", type=Path, help="Existing single-run artifact directory; otherwise run_1 is reference")
    parser.add_argument("--checkpoint-mode", choices=("exact", "build-metadata"), default="exact",
                        help="Default requires whole-file equality; build-metadata explicitly permits only offline build ID/checksum differences")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--hz", type=float, default=10, help="Runner publication frequency, one frequency per series")
    parser.add_argument("--consumer-ms", type=float, default=1, help="Runner consumer polling wait")
    parser.add_argument("--gui-display-hz", type=float, help="Pass --display-hz to a new GUI; omit for frozen legacy GUI")
    args = parser.parse_args()
    if args.repeat < 1 or not 1 <= args.hz <= 60 or not 0 <= args.consumer_ms <= 60000:
        parser.error("repeat >= 1, 1 <= hz <= 60, 0 <= consumer-ms <= 60000 required")
    if args.gui_display_hz is not None and (args.mode != "gui" or not 1 <= args.gui_display_hz <= 60):
        parser.error("gui-display-hz requires gui mode and a value in [1,60]")
    args.exe = args.exe.resolve(); args.fixture = args.fixture.resolve(); args.out = args.out.resolve()
    reference = args.reference.resolve() if args.reference else None
    if not args.exe.is_file() or not args.fixture.is_file() or (reference and not reference.is_dir()):
        parser.error("Executable, fixture and optional reference must exist")
    # Same lock as run.py: two cooperating scripts cannot measure concurrently.
    lock = Path(__file__).resolve().parent / ".benchmark.lock"
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(str(os.getpid()))
        args.out.mkdir(parents=True, exist_ok=False)
        rows = []
        report = {"started_utc": datetime.now(timezone.utc).isoformat(), "mode": args.mode,
                  "executable": str(args.exe), "exe_sha256": sha(args.exe),
                  "fixture": str(args.fixture), "fixture_sha256": sha(args.fixture),
                  "reference": str(reference) if reference else None,
                  "checkpoint_mode": args.checkpoint_mode,
                  "host": {"platform": platform.platform(), "machine": platform.machine(),
                           "processor": platform.processor(), "logical_cpu_count": os.cpu_count(),
                           "python": platform.python_version()},
                  "runs": rows,
                  "limits": "Sequential fresh processes; process_seconds includes startup and exports. Timing intervals differ between direct CLI, headless runner, and GUI; do not treat their ratio as solver speedup. Collect hardware/power/load and exact build flags separately."}
        for index in range(args.repeat):
            output = args.out / f"run_{index + 1}"; output.mkdir()
            command = [str(args.exe), str(args.fixture)]
            if args.mode == "runner":
                command += [str(output), str(args.hz), str(args.consumer_ms)]
            elif args.mode == "gui":
                command += ["--benchmark", str(output)]
                if args.gui_display_hz is not None:
                    command += ["--display-hz", str(args.gui_display_hz)]
            else:
                command += [str(output)]
            began = time.perf_counter()
            with (output / "process.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            elapsed = time.perf_counter() - began
            if result.returncode:
                raise RuntimeError(f"Exit {result.returncode}; see {output / 'process.log'}")
            timing_file = "timing.json" if args.mode == "cli" else "benchmark.json"
            timing = json.loads((output / timing_file).read_text(encoding="utf-8"))
            timing.update(process_seconds=elapsed, run=index + 1, command=command)
            if reference is not None:
                comparison = compare(reference, output, args.checkpoint_mode)
                timing["comparison_equal"] = comparison["equal"]
            else:
                reference = output
                report["reference"] = str(reference)
                timing["comparison_equal"] = None
            rows.append(timing)
            key = "solve_seconds" if args.mode == "cli" else "wall_seconds"
            values = [row[key] for row in rows]
            report["summary"] = {"metric": key, "n": len(values), "median": statistics.median(values),
                                 "min": min(values), "max": max(values),
                                 "stdev": statistics.stdev(values) if len(values) > 1 else None}
            (args.out / "runs.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(f"Finished {args.mode} {output.name}: {values[-1]:.6f}s {key}; {elapsed:.3f}s process", flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
