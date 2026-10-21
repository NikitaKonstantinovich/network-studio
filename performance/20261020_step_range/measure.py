"""Sequential equal-window measurements; only dt changes in private JSON copies."""
import argparse
import csv
import hashlib
import json
import math
import statistics
import subprocess
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path):
    with path.open(encoding="utf-8") as file:
        return list(csv.DictReader(file))


def spike_times(path):
    result = {}
    for row in rows(path):
        if row["type"] == "spike":
            result.setdefault(int(row["node_index_1based"]), []).append(float(row["time_s"]))
    return result


def differences(reference, current):
    delta = [b - a for a, b in zip(reference, current)]
    assert len(reference) == len(current)
    return {"max_abs": max(map(abs, delta), default=0),
            "rms": math.sqrt(sum(x * x for x in delta) / len(delta)) if delta else 0}


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, default=root / "build/Release/network_studio_step_tests.exe")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    assert 1 <= args.repeats <= 10
    args.output.mkdir(parents=True, exist_ok=False)
    source_hash = sha(args.config)
    config = json.loads(args.config.read_text(encoding="utf-8"))
    assert len(config["neurons"]) == 500 and len(config["synapses"]) == 15041
    assert not config["astrocytes"] and all(n["model"] == "izhikevich" for n in config["neurons"])
    steps = [25, 50, 100, 250, 500]
    inputs = {}
    for dt in steps:
        copy = dict(config, dt=dt / 1e6)
        assert {k: v for k, v in copy.items() if k != "dt"} == {k: v for k, v in config.items() if k != "dt"}
        path = args.output / f"dt_{dt}.json"
        path.write_text(json.dumps(copy, ensure_ascii=False, indent=2), encoding="utf-8")
        inputs[str(dt)] = {"path": str(path), "sha256": sha(path)}
    executable = args.executable.resolve()
    executable_hash = sha(executable)
    runs = []
    started = time.perf_counter()
    for repeat in range(args.repeats):
        # Rotate order; processes are awaited, never concurrent.
        for dt in steps[repeat % len(steps):] + steps[:repeat % len(steps)]:
            folder = args.output / f"dt_{dt}_repeat_{repeat + 1}"
            command = [str(executable), inputs[str(dt)]["path"], str(folder), str(args.threads), "benchmark"]
            start = time.perf_counter()
            completed = subprocess.run(command, text=True, encoding="utf-8", capture_output=True)
            wall = time.perf_counter() - start
            assert completed.returncode == 0, completed.stdout + completed.stderr
            (folder / "console.txt").write_text(completed.stdout + completed.stderr, encoding="utf-8")
            measured = rows(folder / "timing.csv")
            for window in measured:
                assert (float(window["from"]), float(window["to"])) in ((0., .05), (.05, .1))
            item = {"dt_us": dt, "repeat": repeat + 1, "process_seconds": wall,
                    "command": command, "windows": measured, "checkpoint_sha256": sha(folder / "final.nscp")}
            runs.append(item)
            print(f"{dt} us repeat {repeat+1}: solve {sum(float(w['solve_s']) for w in measured):.6f} s, process {wall:.3f} s", flush=True)
    assert sha(args.config) == source_hash and sha(executable) == executable_hash
    aggregate = []
    for dt in steps:
        selected = [r for r in runs if r["dt_us"] == dt]
        assert len({r["checkpoint_sha256"] for r in selected}) == 1, "Repeated states differ"
        values = [sum(float(w["solve_s"]) for w in r["windows"]) for r in selected]
        counts = [sum(int(w["accepted"]) for w in r["windows"]) for r in selected]
        rejected = [sum(int(w["rejected"]) for w in r["windows"]) for r in selected]
        assert len(set(counts)) == len(set(rejected)) == 1
        median = statistics.median(values)
        aggregate.append({"dt_us": dt, "median_s": median, "minimum_s": min(values), "maximum_s": max(values),
                          "accepted": counts[0], "rejected": rejected[0], "mean_internal_step_us": .1 / counts[0] * 1e6,
                          "estimated_1000_s_hours": median / .1 * 1000 / 3600,
                          "quiet_median_s": statistics.median(float(r["windows"][0]["solve_s"]) for r in selected),
                          "active_median_s": statistics.median(float(r["windows"][1]["solve_s"]) for r in selected),
                          "effective_threads": sorted({int(w["effective_threads"]) for r in selected for w in r["windows"]})})
    for row in aggregate:
        row["speedup_vs_25"] = aggregate[0]["median_s"] / row["median_s"]
    baseline = args.output / "dt_25_repeat_1"
    state0 = [float(r["value"]) for r in rows(baseline / "final_state.csv")]
    summary0 = json.loads((baseline / "result_summary.json").read_text())
    spikes0 = spike_times(baseline / "result_events.csv")
    accuracy = {}
    for dt in steps:
        folder = args.output / f"dt_{dt}_repeat_1"
        state = [float(r["value"]) for r in rows(folder / "final_state.csv")]
        summary = json.loads((folder / "result_summary.json").read_text())
        spikes = spike_times(folder / "result_events.csv")
        same_counts = summary["neuron_spikes"] == summary0["neuron_spikes"]
        matched = [abs(a - b) for node in spikes0 for a, b in zip(spikes0[node], spikes.get(node, []))]
        deliver0 = rows(baseline / "result_deliveries.csv")
        deliver = rows(folder / "result_deliveries.csv")
        arrivals0, arrivals = {}, {}
        for data, target in ((deliver0, arrivals0), (deliver, arrivals)):
            for event in data:
                target.setdefault(int(event["synapse_index_1based"]), []).append(float(event["t_arrival_s"]))
        matching_arrivals = [abs(a-b) for edge, times in arrivals0.items() for a, b in zip(times, arrivals.get(edge, []))]
        stimuli_exact = all((baseline / ("result_" + name + ".csv")).read_bytes() ==
                            (folder / ("result_" + name + ".csv")).read_bytes()
                            for name in ("stimuli", "shared_stimuli"))
        sample_clock_exact = [r["time_s"] for r in rows(baseline / "result_trace.csv")] == [
            r["time_s"] for r in rows(folder / "result_trace.csv")]
        assert stimuli_exact and sample_clock_exact, "Input pulses or recording clock changed"
        indices = {"voltage_mV": range(0, 2000, 4), "u": range(1, 2000, 4),
                   "resource_u": range(2000, len(state), 4), "resource_x": range(2001, len(state), 4),
                   "active_y": range(2002, len(state), 4), "glutamate": range(2003, len(state), 4)}
        accuracy[str(dt)] = {
            "same_spikes_per_neuron": same_counts, "spikes": sum(summary["neuron_spikes"]),
            "deliveries": len(deliver), "same_deliveries_per_edge": {k: len(v) for k,v in arrivals.items()} == {k: len(v) for k,v in arrivals0.items()},
            "max_ordinal_spike_difference_s": max(matched, default=0),
            "max_ordinal_arrival_difference_s": max(matching_arrivals, default=0),
            "stimulus_csv_byte_exact": stimuli_exact, "sample_clock_exact": sample_clock_exact,
            "weights": differences(summary0["final_synapse_weights"], summary["final_synapse_weights"]),
            "state": {name: differences([state0[i] for i in ids], [state[i] for i in ids]) for name, ids in indices.items()},
            "inhibitory_weights_exact": all(summary["final_synapse_weights"][i] == summary0["final_synapse_weights"][i]
                for i, s in enumerate(config["synapses"]) if config["neurons"][s["source"]]["inhibitory"])
        }
    report = {"input": str(args.config.resolve()), "input_sha256": source_hash, "copies": inputs,
              "executable_sha256": executable_hash, "threads_requested": args.threads, "repeats": args.repeats,
              "model_seconds_each_run": .1, "runs": runs, "aggregate": aggregate, "accuracy_vs_25us": accuracy,
              "elapsed_seconds": time.perf_counter() - started,
              "scope": "Two early 50 ms windows, 0..0.1 model s per run. 1000 s estimates are linear projections, not long experiments."}
    (args.output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    with (args.output / "aggregate.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=list(aggregate[0]))
        writer.writeheader()
        writer.writerows(aggregate)
    print("PASS sequential repetitions, input preservation and exact repeated checkpoints", flush=True)


if __name__ == "__main__":
    main()
