"""Measure archived optimization stages sequentially on one unchanged 50 ms fixture.

Default is a read-only plan. --execute requires a fresh output directory. No build,
test, checkpoint loading, or mutation of archived evidence is performed here.
"""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
PERFORMANCE = HERE.parent
ROOT = PERFORMANCE.parent
SINGLE = PERFORMANCE / "20261019_single_thread"
RUNNER = PERFORMANCE / "20261019_runner"
PARALLEL = PERFORMANCE / "20261019_parallel"
FIXTURE = SINGLE / "profile_fixture.json"
TIMEOUT_SECONDS = 240
sys.path.insert(0, str(PERFORMANCE))
from run_runner import compare, equal_files, sha

_spec = importlib.util.spec_from_file_location("stage_review_memory", PARALLEL / "measure.py")
_memory_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_memory_module)
run_process = _memory_module.run_process


def stage(label, exe, *, threads=None, gui=False, display_hz=None,
          modern_gui=False, profile=False, sources=()):
    return {"label": label, "executable": str(exe), "mode": "gui" if gui else "direct",
            "threads_requested": threads, "display_hz": display_hz,
            "modern_gui": modern_gui, "profile_enabled_expected": profile,
            "source_paths": [str(p) for p in sources]}


def definitions():
    benchmark = "network_studio_benchmark.exe"
    gui = "network_studio.exe"
    original_sources = (SINGLE / "original-source.zip", SINGLE / "baseline_instrumentation.patch")
    compact_sources = (SINGLE / "optimized_sources.zip", SINGLE / "source_archives.json")
    runner_sources = (RUNNER / "source_snapshot.zip", RUNNER / "build_manifest.json")
    final_sources = (PARALLEL / "source_snapshot.zip", PARALLEL / "build_manifest.json")
    # Intermediate executables were archived in place; do not attribute the final
    # source snapshot to these earlier kernels. Their provenance is explicitly limited.
    intermediate_sources = (PARALLEL / "provenance.json", PARALLEL / "ARCHITECTURE_RU.md")
    control = HERE / "control_no_parallel_roots" / "source"
    return [
        stage("s0_baseline", SINGLE / "baseline-release" / benchmark, sources=original_sources),
        stage("s1_caches", SINGLE / "stage1-release" / benchmark,
              sources=(SINGLE / "stage1_sources.zip", SINGLE / "source_archives.json")),
        stage("s2_compact", SINGLE / "optimized-release" / benchmark, sources=compact_sources),
        stage("s3_runner_direct", RUNNER / "runner-release" / benchmark, sources=runner_sources),
        stage("p1_resources4", PARALLEL / "stage1_resources/benchmark.exe", threads=4, sources=intermediate_sources),
        stage("p2_currents4", PARALLEL / "stage2_currents/benchmark.exe", threads=4, sources=intermediate_sources),
        stage("p3_stdp4", PARALLEL / "stage3_stdp/benchmark.exe", threads=4, sources=intermediate_sources),
        stage("control_no_roots4", control / "build/control/Release" / benchmark, threads=4,
              sources=(control / "network.cpp", control / "integrator.cpp", control / "numerics/integrator.h")),
        stage("p5_final4", PARALLEL / "parallel-release" / benchmark, threads=4, sources=final_sources),
        stage("p5_final8", PARALLEL / "parallel-release" / benchmark, threads=8, sources=final_sources),
        stage("profile_stage4_4", PARALLEL / "profile_stage4/benchmark.exe", threads=4, profile=True,
              sources=(PARALLEL / "profile_stage4/checkpoint_build.h",) + intermediate_sources),
        stage("profile_final4", PARALLEL / "profile_final/benchmark.exe", threads=4, profile=True,
              sources=(PARALLEL / "profile_final/checkpoint_build.h",) + final_sources),
        stage("gui_baseline", SINGLE / "baseline-release" / gui, gui=True, sources=original_sources),
        stage("gui_compact", SINGLE / "optimized-release" / gui, gui=True, sources=compact_sources),
        stage("gui_runner", RUNNER / "runner-release" / gui, gui=True, display_hz=10, sources=runner_sources),
        stage("gui_final8", PARALLEL / "parallel-release" / gui, gui=True, display_hz=10,
              threads=8, modern_gui=True, sources=final_sources),
    ]


class DeadlineSubprocess:
    """Private adapter around the unchanged memory sampler's subprocess module.

    A timer owns the actual benchmark Popen object (not a PID guessed afterwards),
    kills it at the deadline, and lets run_process reap it and save memory counters.
    These executables create worker threads, not child processes.
    """
    def __init__(self, timeout):
        self.timeout = timeout
        self.child = None
        self.timer = None
        self.timed_out = False
        self.kill_error = None

    def __getattr__(self, name):
        return getattr(subprocess, name)

    def Popen(self, *args, **kwargs):
        self.child = subprocess.Popen(*args, **kwargs)
        self.timer = threading.Timer(self.timeout, self.expire)
        self.timer.daemon = True
        self.timer.start()
        return self.child

    def expire(self):
        if self.child.poll() is None:
            self.timed_out = True
            try:
                self.child.kill()
            except OSError as exc:
                self.kill_error = str(exc)

    def close(self):
        if self.timer is not None:
            self.timer.cancel()
            self.timer.join()
        if self.child is not None and self.child.poll() is None:
            self.child.kill()
            self.child.wait(timeout=10)


def measured_process(command, output, interval):
    guard = DeadlineSubprocess(TIMEOUT_SECONDS)
    previous = _memory_module.subprocess
    _memory_module.subprocess = guard
    try:
        result = run_process(command, output, interval)
        if guard.timed_out:
            raise TimeoutError(f"Benchmark exceeded {TIMEOUT_SECONDS} seconds")
        return result
    except BaseException as exc:
        if guard.timed_out:
            with (output / "process.log").open("a", encoding="utf-8") as log:
                log.write(f"\nDriver timeout: killed benchmark after {TIMEOUT_SECONDS} seconds.\n")
            raise TimeoutError(f"Benchmark exceeded {TIMEOUT_SECONDS} seconds; process killed") from exc
        raise
    finally:
        try:
            guard.close()
        finally:
            _memory_module.subprocess = previous
            write_json(output / "deadline.json", {"timeout_seconds": TIMEOUT_SECONDS,
                       "timed_out": guard.timed_out, "kill_error": guard.kill_error,
                       "returncode": guard.child.returncode if guard.child else None})


def write_json(path, value):
    # Only files created in this invocation's fresh output tree are replaced.
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def artifact(path):
    item = {"path": str(path), "exists": path.exists()}
    if path.is_file():
        item.update(size_bytes=path.stat().st_size, sha256=sha(path))
    return item


def preflight_binaries(selected):
    expected = {}
    single_manifest = SINGLE / "final_builds.json"
    for build in json.loads(single_manifest.read_text(encoding="utf-8"))["builds"].values():
        for name, digest in build["executables"].items():
            expected[(SINGLE / build["archived_path"] / name).resolve()] = (digest, single_manifest)
    for base, directory in ((RUNNER, "runner-release"), (PARALLEL, "parallel-release")):
        manifest = base / "build_manifest.json"
        for name, entry in json.loads(manifest.read_text(encoding="utf-8"))["executables"].items():
            expected[(base / directory / name).resolve()] = (entry["sha256"], manifest)
    for directory in ("profile_stage4", "profile_final"):
        evidence = PARALLEL / directory / "runs.json"
        data = json.loads(evidence.read_text(encoding="utf-8"))
        expected[(PARALLEL / directory / "benchmark.exe").resolve()] = (data["binaries"]["new"]["sha256"], evidence)
    results = {}
    for item in selected:
        path = Path(item["executable"]).resolve()
        result = artifact(path)
        if path in expected:
            digest, manifest = expected[path]
            result.update(expected_sha256=digest, historical_manifest=str(manifest),
                          matches_historical_manifest=result["sha256"] == digest)
            if result["sha256"] != digest:
                raise RuntimeError(f"Archived executable hash differs from its historical manifest: {path}")
        else:
            result.update(expected_sha256=None, historical_manifest=None,
                          provenance_limit="SHA-256 captured at this review's preflight; no historical executable digest is assigned here. Intermediate p1/p2/p3 source identity is not independently established; control is a newly built isolated ablation.")
        results[item["label"]] = result
    return results


def command_for(item, output):
    command = [item["executable"]]
    if item["modern_gui"]:
        command.append("--config")
    command.append(str(FIXTURE))
    if item["mode"] == "gui":
        command += ["--benchmark", str(output)]
        if item["display_hz"] is not None:
            command += ["--display-hz", str(item["display_hz"])]
        if item["threads_requested"] is not None:
            command += ["--compute-threads", str(item["threads_requested"])]
    else:
        command.append(str(output))
        if item["threads_requested"] is not None:
            command.append(str(item["threads_requested"]))
    return command


def validate_timing(item, timing, output):
    model = timing.get("model_seconds", timing.get("model_final_seconds"))
    if model != .05 or timing.get("steps") != 2000:
        raise RuntimeError(f"Wrong completed trajectory: model={model}, steps={timing.get('steps')}")
    for name, expected in (("events", 5771), ("deliveries", 172612)):
        if name in timing and timing[name] != expected:
            raise RuntimeError(f"Unexpected {name}: {timing[name]} != {expected}")
    if "model_start_seconds" in timing and timing["model_start_seconds"] != 0:
        raise RuntimeError("GUI did not start at model time zero")
    requested = item["threads_requested"]
    actual_requested = timing.get("requested_threads", timing.get("compute_threads"))
    effective = timing.get("effective_threads", timing.get("effective_compute_threads"))
    if requested is not None and (actual_requested != requested or effective != requested):
        raise RuntimeError(f"Wrong requested/effective thread counts: {actual_requested}/{effective}, expected {requested}")
    if requested is None and (actual_requested not in (None, 1) or effective not in (None, 1)):
        raise RuntimeError("Historical serial stage unexpectedly reported parallel execution")
    if item["display_hz"] is not None and timing.get("display_hz") != item["display_hz"]:
        raise RuntimeError("GUI publication frequency differs from the plan")
    profile_path = output / "profile.json"
    profile = json.loads(profile_path.read_text(encoding="utf-8")) if profile_path.is_file() else None
    # Historical GUI executables do not export profile.json. Record that fact;
    # all direct and explicitly profiled executables must report their flag.
    if profile is None:
        if item["mode"] == "direct" or item["profile_enabled_expected"]:
            raise RuntimeError("Missing profile.json; profiling status cannot be verified")
    elif profile.get("enabled") is not item["profile_enabled_expected"]:
        raise RuntimeError("Profiling-enabled executable was mislabeled in the plan")
    return {"actual_model_seconds": model,
            "validated_counts": {k: timing.get(k) for k in ("steps", "events", "deliveries")},
            "profile_enabled": profile["enabled"] if profile is not None else None, "profile": profile}


def comparison(reference, actual, policy, name):
    destination = actual / ("comparison_" + name + ".json")
    try:
        report = compare(reference, actual, policy)
    finally:
        generated = actual / "comparison.json"
        if generated.is_file():
            generated.replace(destination)
    return {"equal": report["equal"], "reference": str(reference), "checkpoint_mode": policy,
            "report_path": str(destination), "csv_count": len(report["csv"]),
            "checkpoint": report["checkpoint"]}


def moments(values):
    return {"n": len(values), "median": statistics.median(values), "min": min(values),
            "max": max(values), "stdev": statistics.stdev(values) if len(values) > 1 else None}


def summarize(rows):
    summary = {}
    for label in dict.fromkeys(row["label"] for row in rows):
        group = [row for row in rows if row["label"] == label]
        metric = "solve_seconds" if group[0]["mode"] == "direct" else "wall_seconds"
        summary[label] = {"metric": metric, **moments([row[metric] for row in group]),
                          "process_seconds": moments([row["process_seconds"] for row in group])}
        memory = [row["memory"].get("peak_working_set_bytes_observed") for row in group]
        if all(value is not None for value in memory):
            summary[label]["memory_peak_working_set_bytes"] = moments(memory)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    all_stages = definitions()
    labels = [item["label"] for item in all_stages]
    parser.add_argument("--out", type=Path, required=True, help="Fresh result directory; never overwrite a series")
    parser.add_argument("--select", nargs="+", choices=labels, default=labels)
    parser.add_argument("--reference", type=Path, help="Existing s0_baseline/run_1 from the same fixture; enables subsets without rerunning s0")
    parser.add_argument("--repeat", type=int, default=3, help="Repetitions per stage; gui_baseline is always one")
    parser.add_argument("--memory-ms", type=float, default=100)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1 or not 10 <= args.memory_ms <= 10000:
        parser.error("repeat >= 1 and memory-ms in [10,10000] required")
    args.out = args.out.resolve()
    reference = args.reference.resolve() if args.reference else args.out / "s0_baseline/run_1"
    selected = [item for item in all_stages if item["label"] in args.select]
    if args.reference is None and "s0_baseline" not in args.select:
        parser.error("--select must include s0_baseline unless --reference is supplied")
    if args.reference is not None and "s0_baseline" in args.select:
        parser.error("With --reference, omit s0_baseline from --select to keep the baseline unambiguous")
    plan = []
    for item in selected:
        for repeat in range(1, (1 if item["label"] == "gui_baseline" else args.repeat) + 1):
            output = args.out / item["label"] / f"run_{repeat}"
            plan.append({**item, "repeat": repeat, "output": str(output), "command": command_for(item, output)})
    if not args.execute:
        print(json.dumps({"execute": False, "fixture": str(FIXTURE), "reference": str(reference),
                          "model_seconds_each": .05, "timeout_seconds_each": TIMEOUT_SECONDS,
                          "process_count": len(plan), "plan": plan}, indent=2))
        return
    if os.name != "nt":
        parser.error("The imported process-memory sampler requires Windows")
    if args.out.exists():
        parser.error("--out already exists; use a fresh directory")
    for item in selected:
        if not Path(item["executable"]).is_file():
            parser.error(f"Executable missing; prepare/build it separately: {item['executable']}")
    config = json.loads(FIXTURE.read_text(encoding="utf-8"))
    if (config["duration"], config["dt"], config["noise_interval"], config["seed"]) != (.05, .000025, .001, 706):
        parser.error("The common archived fixture no longer has the expected parameters")
    if args.reference:
        ref_timing = json.loads((reference / "timing.json").read_text(encoding="utf-8"))
        validate_timing(all_stages[0], ref_timing, reference)
        if not (reference / "final.nscp").is_file() or not list(reference.glob("result_*.csv")):
            parser.error("External baseline lacks full checkpoint/CSV artifacts")
    report = {"schema": 1, "status": "running", "started_utc": datetime.now(timezone.utc).isoformat(),
              "fixture": {**artifact(FIXTURE), "duration": config["duration"], "dt": config["dt"],
                          "seed": config["seed"], "noise_interval": config["noise_interval"],
                          "neurons": len(config["neurons"]), "synapses": len(config["synapses"]),
                          "provenance": str(SINGLE / "provenance.json")},
              "reference": str(reference), "stage_definitions": selected, "plan": plan,
              "host": {"platform": platform.platform(), "processor": platform.processor(),
                       "logical_cpu_count": os.cpu_count(), "python": platform.python_version()},
              "preflight": {"binaries": preflight_binaries(selected),
                            "sources": {item["label"]: [artifact(Path(p)) for p in item["source_paths"]] for item in selected},
                            "driver": artifact(Path(__file__).resolve())},
              "timeout_seconds_each": TIMEOUT_SECONDS, "runs": [], "failures": [], "summary": {},
              "limits": "Sequential fresh processes and fixed stage order. No thermal/background-load control. Direct solve_seconds and GUI wall_seconds are distinct intervals; profiled stages are diagnostics, not production-speed estimates. Memory covers the whole child process including startup/export. Historical intermediate binaries have limited source provenance. Cross-build checkpoint comparisons exclude only build metadata offline; no loader compatibility checks are bypassed."}
    lock = PERFORMANCE / ".benchmark.lock"
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        with os.fdopen(descriptor, "w", encoding="ascii") as stream:
            stream.write(str(os.getpid()))
        args.out.mkdir(parents=True, exist_ok=False)
        write_json(args.out / "runs.json", report)
        first_outputs = {}
        for item in plan:
            output = Path(item["output"])
            output.mkdir(parents=True, exist_ok=False)
            began = time.perf_counter()
            print(f"Starting {item['label']} repeat {item['repeat']}", flush=True)
            try:
                # Detect replacement of binaries or fixture during a long series.
                executable_sha = sha(Path(item["executable"]))
                if executable_sha != report["preflight"]["binaries"][item["label"]]["sha256"] or sha(FIXTURE) != report["fixture"]["sha256"]:
                    raise RuntimeError("Executable or common fixture changed after preflight")
                elapsed, memory = measured_process(item["command"], output, args.memory_ms / 1000)
                timing_file = "timing.json" if item["mode"] == "direct" else "benchmark.json"
                timing = json.loads((output / timing_file).read_text(encoding="utf-8"))
                verified = validate_timing(item, timing, output)
                comparisons = {"baseline": comparison(reference, output, "exact" if output == reference else "build-metadata", "baseline")}
                comparisons["baseline"]["is_reference"] = output == reference
                if item["repeat"] > 1:
                    comparisons["repeat"] = comparison(first_outputs[item["label"]], output, "exact", "repeat")
                if item["label"] == "s3_runner_direct" and "s2_compact" in first_outputs:
                    comparisons["compact_runner"] = comparison(first_outputs["s2_compact"], output, "exact", "compact_runner")
                if item["mode"] == "direct":
                    left, right = reference / "numbers.bin", output / "numbers.bin"
                    if left.is_file() and right.is_file():
                        identical = equal_files(left, right)
                        comparisons["direct_numbers"] = {"equal": identical, "reference": str(left),
                            "actual": str(right), "reference_sha256": sha(left), "actual_sha256": sha(right)}
                        write_json(output / "comparison_numbers.json", comparisons["direct_numbers"])
                        if not identical:
                            raise RuntimeError("numbers.bin differs from s0 baseline")
                    else:
                        comparisons["direct_numbers"] = {"equal": None, "reason": "numbers.bin unavailable in one executable's output"}
                row = {**timing, **item, **verified, "status": "complete", "process_seconds": elapsed,
                       "measurement_mode": timing.get("mode", "direct_solver"),
                       "exe_sha256": executable_sha, "fixture_sha256": report["fixture"]["sha256"],
                       "memory": {k: v for k, v in memory.items() if k != "timeline"}, "comparisons": comparisons}
                report["runs"].append(row)
                first_outputs.setdefault(item["label"], output)
                report["summary"] = summarize(report["runs"])
                write_json(args.out / "runs.json", report)
                metric = "solve_seconds" if item["mode"] == "direct" else "wall_seconds"
                print(f"Finished {item['label']}: {timing[metric]:.6f}s {metric}; {elapsed:.3f}s process; comparisons passed", flush=True)
            except BaseException as exc:
                failure = {**item, "status": "failed", "error_type": type(exc).__name__, "error": str(exc),
                           "driver_elapsed_seconds": time.perf_counter() - began,
                           "failed_utc": datetime.now(timezone.utc).isoformat()}
                for name in ("memory.json", "deadline.json"):
                    if (output / name).is_file():
                        failure[name[:-5]] = json.loads((output / name).read_text(encoding="utf-8"))
                report["failures"].append(failure)
                report["status"] = "failed"
                write_json(output / "failure.json", failure)
                write_json(args.out / "runs.json", report)
                raise
        report["status"] = "complete"
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(args.out / "runs.json", report)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
