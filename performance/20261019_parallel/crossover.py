"""Synthetic threshold/crossover fixtures; print plan by default, measure only with --execute."""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics

from measure import HERE, ROOT, compare, run_process, sha
from prepare import write_new_or_same

SIZES = (64, 128, 256)
DURATIONS = (.005, .02)
SEED = 706
FIXTURE_REVISION = 2
PROVENANCE = HERE / "crossover_provenance_v2.json"


def ranking(size, target, source, purpose="topology"):
    return hashlib.sha256(f"parallel-crossover-v1|{SEED}|{size}|{target}|{source}|{purpose}".encode("ascii")).digest()


def fixture_path(size, duration):
    return HERE / "fixtures" / f"crossover_v{FIXTURE_REVISION}_{size}n_{size * 32}s_{duration * 1000:g}ms.json"


def prepare_fixtures():
    source = ROOT / "performance/20261019_single_thread/fixture.json"
    raw = source.read_bytes(); original = json.loads(raw)
    previous = json.loads((source.parent / "provenance.json").read_text(encoding="utf-8-sig"))
    if hashlib.sha256(raw).hexdigest() != previous["fixture_sha256"]:
        raise RuntimeError("Original fixture checksum changed")
    populations = [[i for i, neuron in enumerate(original["neurons"]) if neuron["inhibitory"] == inhibitory]
                   for inhibitory in (False, True)]
    outgoing = [[] for _ in original["neurons"]]
    for edge in original["synapses"]:
        outgoing[edge["source"]].append(edge)
    if any(not edges for edges in outgoing):
        raise RuntimeError("Each copied presynaptic neuron must have an original outgoing edge prototype")
    entries = []
    for size in SIZES:
        excitation = 4 * size // 5
        indices = populations[0][:excitation] + populations[1][:size - excitation]
        data = {key: copy.deepcopy(value) for key, value in original.items() if key not in
                ("neurons", "synapses", "astrocytes", "gaps", "stages", "generation", "stimulators")}
        data["neurons"] = [copy.deepcopy(original["neurons"][i]) for i in indices]
        data["astrocytes"] = []; data["gaps"] = []; data["stages"] = []
        data["generation"] = []; data["stimulators"] = []
        # Presence of the optional shared-stimulator section also requires its
        # ID counter and activity-window fields, including in older schemas.
        data["next_stimulator_id"] = "1"
        data["activity_horizon"] = 1
        data["noise_sigma"] = 5; data["noise_interval"] = .001; data["seed"] = SEED
        data["record_all"] = False
        data["record_neurons"] = [i for i in original["record_neurons"] if i < size]
        data["record_synapses"] = list(range(4))
        data["analysis"]["neurons"] = []
        data["analysis"]["reference_neuron"] = 0
        data["analysis"]["min_neurons"] = min(data["analysis"]["min_neurons"], size)
        edges = []
        for target in range(size):
            sources = sorted((i for i in range(size) if i != target), key=lambda i: ranking(size, target, i))[:32]
            for source_index in sources:
                prototypes = outgoing[indices[source_index]]
                choice = int.from_bytes(ranking(size, target, source_index, "weight"), "little") % len(prototypes)
                edge = copy.deepcopy(prototypes[choice])
                edge["source"] = source_index; edge["target"] = target; edge["astro"] = -1
                inhibitory = data["neurons"][source_index]["inhibitory"]
                if edge["model"] != "tsodyks_markram" or not edge["enabled"]:
                    raise RuntimeError("Crossover prototype must be enabled TM")
                if inhibitory and (edge["weight"] >= 0 or edge["stdp"]):
                    raise RuntimeError("Inhibitory prototype polarity/plasticity changed")
                if not inhibitory and (edge["weight"] < 0 or not edge["stdp"] or edge["stdp_rule"] != "multiplicative"):
                    raise RuntimeError("Excitatory prototype polarity/plasticity changed")
                edges.append(edge)
        data["synapses"] = edges
        indegrees = [0] * size
        for edge in edges:
            indegrees[edge["target"]] += 1
        assert len(edges) == 32 * size
        assert all(degree == 32 for degree in indegrees)
        assert len({(edge["source"], edge["target"]) for edge in edges}) == len(edges)
        assert all(edge["source"] != edge["target"] for edge in edges)
        assert all(n["model"] == "izhikevich" for n in data["neurons"])
        for duration in DURATIONS:
            data["name"] = f"Synthetic parallel crossover / {size} Izh / {size * 32} TM / {duration:g}s"
            data["duration"] = duration
            encoded = (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
            path = fixture_path(size, duration); write_new_or_same(path, encoded)
            entries.append({"path": path.relative_to(HERE).as_posix(), "sha256": hashlib.sha256(encoded).hexdigest(),
                            "bytes": len(encoded), "neurons": size, "excitatory": excitation, "inhibitory": size - excitation,
                            "original_neuron_indices": indices, "synapses": len(edges), "indegree_each": 32,
                            "stdp_synapses": sum(edge["stdp"] for edge in edges), "duration": duration,
                            "expected_macro_steps": round(duration / data["dt"]),
                            "static_parallel_eligibility": "All Izh, all TM, no astrocytes, synapses >= 2048; effective_threads must also be verified in a measured process"})
    report = {"prepared_utc": datetime.now(timezone.utc).isoformat(), "revision": FIXTURE_REVISION,
              "revision_reason": "Version 1 added an empty stimulators section without its required next_stimulator_id/activity_horizon fields and failed parsing before any solve. V2 adds string ID 1 and default one-second activity horizon. V1 files, provenance and failed attempt remain unchanged.",
              "previous_provenance": "crossover_provenance.json", "source": "../20261019_single_thread/fixture.json",
              "source_sha256": hashlib.sha256(raw).hexdigest(), "synthetic": True,
              "topology": "For each ascending target, choose 32 distinct non-self sources by ascending SHA256 ranking(seed706,size,target,source,topology). Retain that unsorted hash order in the configuration.",
              "edge_parameters": "Choose an original edge of the mapped presynaptic neuron using the independent weight hash modulo its original outgoing edge count. Copy all its fields except new source/target and astro=-1; preserve E/I sign, plasticity rule and kinetic parameters.",
              "initial_state": "Copy selected neurons exactly; simulator initializes resources and STDP traces through ordinary constructor. No checkpoint state is transplanted.",
              "input": "Global sigma5, interval1ms, seed706; individual inputs unchanged from original zero-input neurons",
              "removed": ["stages", "shared stimulators", "original 500-neuron generation metadata", "astrocytes", "gaps"],
              "sampling": "Original dt25us/sample2ms; record_all=false, original valid first six neurons and first four edges",
              "fixtures": entries, "status": "Synthetic fixtures prepared; no solver, build, test or benchmark run"}
    PROVENANCE.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(entries)} synthetic crossover fixtures")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true", help="Generate fixtures only; cannot be combined with --execute")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--duration", type=float, choices=DURATIONS, default=.02)
    parser.add_argument("--sizes", nargs="+", type=int, choices=SIZES, default=list(SIZES))
    parser.add_argument("--threads", nargs="+", type=int, choices=(1, 2, 4, 8), default=[1, 2, 4, 8])
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--memory-ms", type=float, default=100)
    parser.add_argument("--new-dir", type=Path, default=ROOT / "build/parallel/Release")
    parser.add_argument("--out", type=Path, default=HERE / "crossover_runs")
    args = parser.parse_args()
    if args.prepare:
        if args.execute:
            parser.error("--prepare cannot run benchmarks")
        prepare_fixtures(); return
    if args.repeat < 1 or 1 not in args.threads or not 10 <= args.memory_ms <= 10000:
        parser.error("repeat >= 1, include threads1, memory-ms in [10,10000]")
    exe = args.new_dir.resolve() / "network_studio_benchmark.exe"
    args.out = args.out.resolve()
    threads = list(dict.fromkeys([1] + args.threads))
    plan = []
    for size in list(dict.fromkeys(args.sizes)):
        for count in threads:
            for repetition in range(1, args.repeat + 1):
                output = args.out / f"neurons_{size}" / f"threads_{count}" / f"run_{repetition}"
                fixture = fixture_path(size, args.duration)
                plan.append({"neurons": size, "synapses": size * 32, "threads": count, "repeat": repetition,
                             "fixture": str(fixture), "output": str(output),
                             "command": [str(exe), str(fixture), str(output), str(count)]})
    if not args.execute:
        print(json.dumps({"execute": False, "model_seconds_each": args.duration, "process_count": len(plan), "plan": plan}, indent=2)); return
    if os.name != "nt" or not exe.is_file() or any(not Path(row["fixture"]).is_file() for row in plan):
        parser.error("Requires Windows, built executable and prepared fixtures")
    lock = HERE.parent / ".benchmark.lock"
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(str(os.getpid()))
        args.out.mkdir(parents=True, exist_ok=False)
        report = {"started_utc": datetime.now(timezone.utc).isoformat(), "synthetic": True,
                  "provenance": str(PROVENANCE), "exe": str(exe), "exe_sha256": sha(exe),
                  "model_seconds_each": args.duration, "runs": [], "summary": {},
                  "limits": "Direct solver only; same-build serial reference. Fixed sequential order, no thermal control. Memory covers whole process. Synthetic crossover evidence is not a laboratory reproduction."}
        for row in plan:
            output = Path(row["output"]); output.mkdir(parents=True)
            print(f"Starting N{row['neurons']} threads{row['threads']} repeat{row['repeat']}", flush=True)
            elapsed, memory = run_process(row["command"], output, args.memory_ms / 1000)
            timing = json.loads((output / "timing.json").read_text(encoding="utf-8"))
            if timing.get("model_seconds") != args.duration or timing.get("steps") != round(args.duration / .000025):
                raise RuntimeError("Crossover actual interval or macro-step count differs")
            if timing.get("requested_threads") != row["threads"] or timing.get("effective_threads") != row["threads"]:
                raise RuntimeError("Crossover did not actually execute the requested thread count")
            timing.update(row, process_seconds=elapsed, fixture_sha256=sha(Path(row["fixture"])),
                          memory={key: value for key, value in memory.items() if key != "timeline"})
            reference = args.out / f"neurons_{row['neurons']}" / "threads_1" / "run_1"
            if output != reference:
                timing["comparison_equal"] = compare(reference, output, "exact")["equal"]
            report["runs"].append(timing)
            group = f"N{row['neurons']}/threads{row['threads']}"
            values = [r["solve_seconds"] for r in report["runs"] if r["neurons"] == row["neurons"] and r["threads"] == row["threads"]]
            report["summary"][group] = {"metric": "solve_seconds", "n": len(values), "median": statistics.median(values),
                                       "min": min(values), "max": max(values), "stdev": statistics.stdev(values) if len(values) > 1 else None}
            (args.out / "runs.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(f"Finished {group}: {values[-1]:.6f}s solve", flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
