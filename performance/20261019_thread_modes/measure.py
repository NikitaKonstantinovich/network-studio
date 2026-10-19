"""Serial fresh-process measurements, exact whole-file comparisons; standard Python only.

Requires the benchmark from the SAME Release build as GUI/CLI. Raw numerical
artifacts live under --raw; timing, commands, checksums and comparisons under --out.
Never overwrites previous run directories. No 150-second lab protocol is implied.
"""
import argparse
import csv
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
COUNTS = (1, 2, 4, 6, 8, 12, 16, 0)
FIXTURES = {"quiet500": "quiet_50ms.json", "burst500": "burst_50ms.json",
            "medium128": "crossover_v2_128n_4096s_20ms.json"}


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def same_bytes(left, right):
    with left.open("rb") as a, right.open("rb") as b:
        while True:
            x, y = a.read(1024 * 1024), b.read(1024 * 1024)
            if x != y:
                return False
            if not x:
                return True


def host():
    result = {"platform": platform.platform(), "logical_cpus": os.cpu_count(),
              "python": platform.python_version(), "processor": platform.processor()}
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            result["processor"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        api = ctypes.WinDLL("kernel32", use_last_error=True).GetLogicalProcessorInformationEx
        api.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        api.restype = ctypes.c_bool
        size = ctypes.c_ulong()
        api(0, None, ctypes.byref(size))  # RelationProcessorCore
        buffer = ctypes.create_string_buffer(size.value)
        if not api(0, buffer, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        position = cores = 0
        while position < size.value:
            length = int.from_bytes(buffer.raw[position + 4:position + 8], "little")
            if length < 8:
                raise RuntimeError("Invalid CPU topology record")
            position += length
            cores += 1
        result["physical_cores"] = cores
    return result


def verify_idle():
    # Cooperating benchmark scripts share the lock below. Also refuse a running
    # app/test/compiler before beginning; don't kill any user-owned process.
    if os.name != "nt":
        return
    listing = subprocess.check_output(["tasklist", "/fo", "csv", "/nh"], creationflags=subprocess.CREATE_NO_WINDOW)
    for row in csv.reader(listing.decode("utf-8", errors="replace").splitlines()):
        if row and (row[0].lower().startswith("network_studio") or row[0].lower() in {"ctest.exe", "cl.exe", "ninja.exe"}):
            raise RuntimeError("Competing app/test/build is running: " + row[0])


def artifacts(directory):
    files = sorted(directory.glob("result_*.csv")) + [directory / "final.nscp", directory / "numbers.bin"]
    if len(files) < 3:
        raise RuntimeError("Missing exported histories")
    return {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in files}


def execute(exe, fixture, target, count):
    target.mkdir(parents=True, exist_ok=False)
    command = [str(exe), str(fixture), str(target), str(count)]
    started = time.perf_counter()
    with (target / "process.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True,
                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    wall = time.perf_counter() - started
    timing = json.loads((target / "timing.json").read_text())
    if timing["requested_threads"] != count or (count and timing["effective_threads"] != count):
        raise RuntimeError("Requested/actual thread mismatch")
    return {"command": command, "process_seconds": wall, **timing, "artifacts": artifacts(target)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", type=Path, default=ROOT / "build/Release/network_studio_benchmark.exe")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--raw", type=Path, required=True)
    p.add_argument("--repeat", type=int, default=3)
    args = p.parse_args()
    if args.repeat < 3:
        p.error("At least three repeats required")
    args.exe, args.out, args.raw = args.exe.resolve(), args.out.resolve(), args.raw.resolve()
    verify_idle()
    lock = HERE.parent / ".benchmark.lock"
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(str(os.getpid()))
        args.out.mkdir(parents=True, exist_ok=False)
        args.raw.mkdir(parents=True, exist_ok=False)
        started = time.perf_counter()
        report = {"started_utc": datetime.now(timezone.utc).isoformat(), "host": host(),
                  "executable": str(args.exe), "exe_sha256": sha(args.exe), "repeats": args.repeat,
                  "fixtures": {}, "warmups": [], "runs": [], "summary": [],
                  "method": "Sequential fresh processes. One unmeasured warmup per workload; rotated mode order each round. Solve timer excludes setup/export and correctness hashing. No affinity/priority changes, thermal or background-load control. Auto=0. Not a 150-second lab run."}
        for name, filename in FIXTURES.items():
            fixture = ROOT / "performance/20261019_parallel/fixtures" / filename
            config = json.loads(fixture.read_text(encoding="utf-8-sig"))
            report["fixtures"][name] = {"path": str(fixture), "sha256": sha(fixture),
                "neurons": len(config["neurons"]), "synapses": len(config["synapses"]),
                "model_seconds": config["duration"], "dt": config["dt"], "noise_sigma": config["noise_sigma"],
                "seed": config["seed"], "stages": "Configured stages are not activated by this benchmark"}
            print(f"Warmup {name}", flush=True)
            warmup = execute(args.exe, fixture, args.raw / name / "warmup", 8)
            report["warmups"].append({"workload": name, **warmup})
            reference = reference_dir = None
            for repeat in range(args.repeat):
                order = COUNTS[repeat:] + COUNTS[:repeat]
                for count in order:
                    print(f"Run {name} threads={count} repeat={repeat + 1}", flush=True)
                    target = args.raw / name / f"threads_{count}_run_{repeat + 1}"
                    row = execute(args.exe, fixture, target, count)
                    if reference is None:
                        assert count == 1
                        reference = row["artifacts"]
                        reference_dir = target
                        assert all(same_bytes(target / filename, args.raw / name / "warmup" / filename) for filename in reference)
                    if row["artifacts"] != reference or warmup["artifacts"] != reference:
                        raise RuntimeError("Numerical files or full checkpoint differ: " + name + "/" + str(count))
                    if not all(same_bytes(reference_dir / filename, target / filename) for filename in reference):
                        raise RuntimeError("Whole-file byte comparison failed")
                    row.update(workload=name, repeat=repeat + 1, exact_match=True)
                    report["runs"].append(row)
                    report["elapsed_seconds"] = time.perf_counter() - started
                    write(args.out / "runs.json", report)
                    print(f"  solve={row['solve_seconds']:.4f}s effective={row['effective_threads']} exact=true", flush=True)
            for count in COUNTS:
                rows = [r for r in report["runs"] if r["workload"] == name and r["requested_threads"] == count]
                seconds = [r["solve_seconds"] for r in rows]
                report["summary"].append({"workload": name, "requested": count,
                    "effective": sorted({r["effective_threads"] for r in rows}), "n": len(rows),
                    "median": statistics.median(seconds), "min": min(seconds), "max": max(seconds),
                    "sd": statistics.stdev(seconds), "model_seconds_per_run": config["duration"],
                    "steps": rows[0]["steps"], "events": rows[0]["events"], "deliveries": rows[0]["deliveries"]})
        for row in report["summary"]:
            eight = next(r for r in report["summary"] if r["workload"] == row["workload"] and r["requested"] == 8)
            row["speedup_over_8"] = eight["median"] / row["median"]
        report["elapsed_seconds"] = time.perf_counter() - started
        report["total_model_seconds_measured"] = sum(r["model_seconds"] for r in report["runs"])
        report["total_model_seconds_warmups"] = sum(r["model_seconds"] for r in report["warmups"])
        write(args.out / "runs.json", report)
        write(args.out / "summary.json", {k: v for k, v in report.items() if k not in {"runs", "warmups"}})
        print(json.dumps(report["summary"], indent=2), flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
