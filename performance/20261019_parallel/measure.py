"""Sequential parallel-solver series. Default prints a plan; --execute explicitly starts measurements."""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from run_runner import compare, sha

PHASES = {"smoke": .005, "diagnostic": .05, "intermediate": .1, "full": .5}


class ProcessMemory:
    """Windows process counters; sampling does not inject code into the child."""
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [(name, ctypes.c_size_t) for name in
                    ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]

    def __init__(self, pid):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        self.close_api = kernel.CloseHandle; self.close_api.argtypes = [wintypes.HANDLE]; self.close_api.restype = wintypes.BOOL
        self.handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        self.api = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
        self.api.argtypes = [wintypes.HANDLE, ctypes.POINTER(self.Counters), wintypes.DWORD]; self.api.restype = wintypes.BOOL
        self.rows = []; self.errors = []; self.start = time.perf_counter()

    def sample(self, after_exit=False):
        counters = self.Counters(); counters.cb = ctypes.sizeof(counters)
        if not self.api(self.handle, ctypes.byref(counters), counters.cb):
            self.errors.append({"after_exit": after_exit, "winerror": ctypes.get_last_error()})
            return False
        self.rows.append({"elapsed_process_seconds": time.perf_counter() - self.start,
                          "working_set_bytes": counters.WorkingSetSize, "peak_working_set_bytes": counters.PeakWorkingSetSize,
                          "private_usage_bytes": counters.PrivateUsage, "peak_pagefile_usage_bytes": counters.PeakPagefileUsage,
                          "after_exit": after_exit})
        return True

    def close(self):
        self.close_api(self.handle)

    def report(self, interval, final_query):
        return {"sampling_seconds": interval, "samples": len(self.rows), "final_query_after_exit_ok": final_query,
                "peak_working_set_bytes_observed": max((r["peak_working_set_bytes"] for r in self.rows), default=None),
                "max_sampled_private_usage_bytes": max((r["private_usage_bytes"] for r in self.rows), default=None),
                "peak_pagefile_usage_bytes_observed": max((r["peak_pagefile_usage_bytes"] for r in self.rows), default=None),
                "query_errors": self.errors, "timeline": self.rows,
                "limits": "Whole child process, including startup/export/checkpoint; not solve-only memory. PeakWorkingSetSize is an OS-maintained maximum through the last successful query. Sampled PrivateUsage can miss intermediate peaks. PeakPagefileUsage is reported by name, not equated to RSS/private peak. No child processes are included."}


def run_process(command, output, interval):
    started = time.perf_counter()
    with (output / "process.log").open("w", encoding="utf-8") as log:
        child = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        monitor = None
        try:
            monitor = ProcessMemory(child.pid)
            while True:
                monitor.sample()
                try:
                    child.wait(timeout=interval)
                    process_elapsed = time.perf_counter() - started
                    break
                except subprocess.TimeoutExpired:
                    pass
            final_query = monitor.sample(after_exit=True)
            memory = monitor.report(interval, final_query)
        except Exception:
            # Never leave a measurement process running if monitoring fails.
            if child.poll() is None:
                child.terminate()
            child.wait()
            raise
        finally:
            if monitor is not None:
                monitor.close()
    (output / "memory.json").write_text(json.dumps(memory, indent=2) + "\n", encoding="utf-8")
    if child.returncode:
        raise RuntimeError(f"Exit {child.returncode}; see {output / 'process.log'}")
    return process_elapsed, memory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=PHASES, default="smoke")
    parser.add_argument("--mode", choices=("cli", "runner"), default="cli")
    parser.add_argument("--new-dir", type=Path, default=ROOT / "build/parallel/Release")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--workloads", nargs="+", choices=("quiet", "burst"), default=["quiet", "burst"])
    parser.add_argument("--threads", nargs="+", type=int, choices=(0, 1, 2, 4, 8), default=[1, 2, 4, 8, 0], help="0 means Auto")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--memory-ms", type=float, default=100)
    parser.add_argument("--execute", action="store_true", help="Explicitly execute the printed plan")
    args = parser.parse_args()
    if args.repeat < 1 or not 10 <= args.memory_ms <= 10000:
        parser.error("repeat >= 1; memory-ms in [10,10000] required")
    if 1 not in args.threads:
        parser.error("Include --threads 1 to establish a new-build serial reference")
    threads = list(dict.fromkeys([1] + args.threads))
    duration = PHASES[args.phase]; args.out = args.out.resolve(); args.new_dir = args.new_dir.resolve()
    exe_name = "network_studio_benchmark.exe" if args.mode == "cli" else "network_studio_runner_benchmark.exe"
    old_exe = ROOT / "performance/20261019_runner/runner-release" / exe_name
    new_exe = args.new_dir / exe_name
    plan = []
    for workload in args.workloads:
        fixture = HERE / "fixtures" / f"{workload}_{duration * 1000:g}ms.json"
        for label, exe, thread_count, repetitions in [("frozen", old_exe, None, 1)] + [(f"threads_{n}", new_exe, n, args.repeat) for n in threads]:
            for repetition in range(1, repetitions + 1):
                output = args.out / workload / label / f"run_{repetition}"
                command = [str(exe), str(fixture), str(output)]
                if args.mode == "runner":
                    command += ["10", "1"]
                if thread_count is not None:
                    command.append(str(thread_count))
                plan.append({"workload": workload, "label": label, "threads_requested": thread_count,
                             "repeat": repetition, "fixture": str(fixture), "output": str(output), "command": command})
    if not args.execute:
        print(json.dumps({"execute": False, "phase": args.phase, "mode": args.mode,
                          "model_seconds_each": duration, "process_count": len(plan), "plan": plan}, indent=2))
        return
    if os.name != "nt":
        parser.error("This memory sampler requires Windows")
    for row in plan:
        if not Path(row["command"][0]).is_file() or not Path(row["fixture"]).is_file():
            parser.error("Builds/fixtures missing; run prepare.py and build first")
    lock = HERE.parent / ".benchmark.lock"
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(str(os.getpid()))
        args.out.mkdir(parents=True, exist_ok=False)
        report = {"started_utc": datetime.now(timezone.utc).isoformat(), "phase": args.phase, "mode": args.mode,
                  "model_seconds_each": duration, "runs": [], "summary": {},
                  "binaries": {"frozen": {"path": str(old_exe), "sha256": sha(old_exe)}, "new": {"path": str(new_exe), "sha256": sha(new_exe)}},
                  "limits": "Single sequential series, fixed thread order. No thermal/background-load control. Memory covers entire process, including exports. Auto's effective thread count must be read from child metrics, not inferred."}
        for row in plan:
            output = Path(row["output"]); output.mkdir(parents=True)
            print(f"Starting {row['workload']} {row['label']} repeat {row['repeat']}", flush=True)
            elapsed, memory = run_process(row["command"], output, args.memory_ms / 1000)
            timing = json.loads((output / ("timing.json" if args.mode == "cli" else "benchmark.json")).read_text(encoding="utf-8"))
            actual_model_time = timing.get("model_seconds", timing.get("model_final_seconds"))
            if actual_model_time != duration:
                raise RuntimeError(f"Actual duration {actual_model_time} differs from requested {duration}")
            if timing.get("steps") != round(duration / .000025):
                raise RuntimeError("Actual macro-step count differs from the 25-microsecond fixture plan")
            timing.update(row, process_seconds=elapsed, fixture_sha256=sha(Path(row["fixture"])),
                          memory={k: v for k, v in memory.items() if k != "timeline"})
            if row["label"] != "frozen":
                reference_label = "frozen" if row["threads_requested"] == 1 and row["repeat"] == 1 else "threads_1"
                reference = args.out / row["workload"] / reference_label / "run_1"
                policy = "build-metadata" if reference_label == "frozen" else "exact"
                comparison = compare(reference, output, policy)
                timing["comparison_equal"] = comparison["equal"]
            report["runs"].append(timing)
            key = "solve_seconds" if args.mode == "cli" else "wall_seconds"
            group = row["workload"] + "/" + row["label"]
            values = [r[key] for r in report["runs"] if r["workload"] + "/" + r["label"] == group]
            report["summary"][group] = {"metric": key, "n": len(values), "median": statistics.median(values),
                                       "min": min(values), "max": max(values),
                                       "stdev": statistics.stdev(values) if len(values) > 1 else None}
            (args.out / "runs.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(f"Finished: {values[-1]:.6f}s {key}, {elapsed:.3f}s process", flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
