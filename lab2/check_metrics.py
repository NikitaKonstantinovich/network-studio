"""Independent checks of Lab 2 graph CSVs and population statistics.

Python 3.9+ standard library only. This script does not import populations.py,
NumPy, or SciPy and never runs or changes a simulation. Events are assigned to
1-ms bins; integer sliding sums independently reproduce the causal 50-ms rate
and 5-s burst rate. Local maxima are found by walking equal-height plateaus.

    python check_metrics.py --graphs-only
    python check_metrics.py
    python check_metrics.py --only k15_seed42_short k30_seed42_short

The explicit burst convention is: peak >5 Hz, plateau midpoint rounded down,
height-priority suppression within 50 ms, earlier peak wins equal heights.
Only complete interior peaks are candidates. Sensitivities use 25/100 ms and
one maximum per connected threshold excursion. These are conventions, not a
claim to reproduce an undocumented NeuroNet peak detector.
"""

import argparse
import bisect
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import sys


HERE = Path(__file__).resolve().parent
N_NEURONS = 500
N_EXCITATORY = 400
BOUNDARY_EPSILON_S = 1e-12


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def approximately(actual, expected, label, absolute=1e-10):
    require(math.isfinite(actual) and math.isclose(actual, expected, rel_tol=1e-11, abs_tol=absolute),
            f"{label}: got {actual!r}, independently expected {expected!r}")


def sliding_sums(values, window):
    total = 0
    result = []
    for index, value in enumerate(values):
        total += value
        if index >= window:
            total -= values[index - window]
        result.append(total)
    return result


def event_bin(timestamp):
    # The first sample k/1000 satisfying timestamp <= k/1000 + epsilon.
    return math.ceil((timestamp - BOUNDARY_EPSILON_S) * 1000.)


def interior_peaks(counts, threshold_count=125):
    result = []
    start = 0
    while start < len(counts):
        end = start
        while end + 1 < len(counts) and counts[end + 1] == counts[start]:
            end += 1
        if (start > 0 and end + 1 < len(counts) and counts[start] > threshold_count
                and counts[start] > counts[start - 1] and counts[end] > counts[end + 1]):
            result.append((start + end) // 2)
        start = end + 1
    return result


def separated_peaks(counts, candidates, distance):
    # Sorted accepted neighbors provide a different implementation from the
    # production script's blocked-array suppression.
    accepted = []
    for peak in sorted(candidates, key=lambda index: (-counts[index], index)):
        insert = bisect.bisect_left(accepted, peak)
        left_ok = insert == 0 or peak - accepted[insert - 1] >= distance
        right_ok = insert == len(accepted) or accepted[insert] - peak >= distance
        if left_ok and right_ok:
            accepted.insert(insert, peak)
    return accepted


def excursion_peaks(counts, threshold_count=125):
    result = []
    position = 0
    while position < len(counts):
        if counts[position] <= threshold_count:
            position += 1
            continue
        best = position
        while position < len(counts) and counts[position] > threshold_count:
            if counts[position] > counts[best]:
                best = position
            position += 1
        result.append(best)
    return result


def self_test():
    bins = [0] * 101
    for timestamp in [.025, .05, .05000000000002, .050000001, .1]:
        bins[event_bin(timestamp)] += 1
    counts = sliding_sums(bins, 50)
    require((counts[24], counts[25], counts[50], counts[74], counts[75], counts[100]) ==
            (0, 1, 3, 4, 3, 2), "Causal boundary self-test failed")
    heights = [0, 126, 126, 0, 125, 125, 0, 130, 0, 129, 0]
    candidates = interior_peaks(heights)
    require(candidates == [1, 7, 9], "Plateau/strict threshold self-test failed")
    require(separated_peaks(heights, candidates, 3) == [1, 7], "Peak separation self-test failed")
    tied = [0, 130, 0, 130, 0]
    require(separated_peaks(tied, interior_peaks(tied), 3) == [1], "Peak tie ordering failed")
    require(interior_peaks([130, 130, 0]) == [], "Incomplete edge plateau was counted")
    burst_bins = [0] * 6001
    burst_bins[1000] = 1
    burst_counts = sliding_sums(burst_bins, 5000)
    require(burst_counts[999] == 0 and burst_counts[1000] == 1 and
            burst_counts[5999] == 1 and burst_counts[6000] == 0, "Five-second boundary self-test failed")
    require(excursion_peaks(heights) == [1, 7, 9], "Excursion self-test failed")
    return {"passed": True, "checks": 7}


def graph_check(path, published):
    degrees = [0] * N_NEURONS
    outgoing = [0] * N_NEURONS
    seen = set()
    magnitudes = []
    excitatory = inhibitory = 0
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == ["source", "target", "weight"], "Unexpected graph columns")
        for row in reader:
            source, target, weight = int(row["source"]), int(row["target"]), float(row["weight"])
            require(0 <= source < N_NEURONS and 0 <= target < N_NEURONS, "Graph endpoint outside 0..499")
            require(source != target, "Unexpected self-connection")
            require((source, target) not in seen, "Duplicate directed connection")
            require(math.isfinite(weight) and 0 <= abs(weight) <= 1, "Weight magnitude outside [0,1]")
            require(weight >= 0 if source < N_EXCITATORY else weight <= 0, "Weight sign contradicts source identity")
            seen.add((source, target))
            degrees[target] += 1
            outgoing[source] += 1
            magnitudes.append(abs(weight))
            excitatory += source < N_EXCITATORY
            inhibitory += source >= N_EXCITATORY
    result = dict(neurons=N_NEURONS, excitatory_neurons=N_EXCITATORY, inhibitory_neurons=N_NEURONS - N_EXCITATORY,
                  synapses=len(seen), indegree_mean=statistics.fmean(degrees), indegree_sd=statistics.stdev(degrees),
                  indegree_min=min(degrees), indegree_max=max(degrees),
                  magnitude_mean=statistics.fmean(magnitudes), magnitude_sd=statistics.stdev(magnitudes),
                  excitatory_edges=excitatory, inhibitory_edges=inhibitory, self_edges=0, duplicate_edges=0)
    for key, expected in result.items():
        approximately(published[key], expected, f"{path.stem}: {key}")
    # A descriptive goodness-of-fit statistic, not a new stochastic acceptance
    # criterion or a claim that one random sample must have exact moments.
    normal = lambda x: .5 * (1 + math.erf((x - .5) / (.1 * math.sqrt(2))))
    lo, hi = normal(0.), normal(1.)
    size = len(magnitudes)
    ks = max(max((index + 1) / size - (normal(value) - lo) / (hi - lo),
                 (normal(value) - lo) / (hi - lo) - index / size)
             for index, value in enumerate(sorted(magnitudes)))
    result.update(passed=True, graph_sha256=digest(path), weight_truncated_normal_ks_distance=ks,
                  neurons_without_outgoing_edges=sum(number == 0 for number in outgoing),
                  topology=published["topology"], seed=published["seed"])
    return result


def read_event_bins(path, duration):
    bins = [0] * (round(duration * 1000) + 1)
    per_neuron = [0] * N_NEURONS
    last_event_per_neuron = [-math.inf] * N_NEURONS
    late_excitatory = late_inhibitory = 0
    comparison_events = 0
    previous = -math.inf
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == ["time_s", "type", "node_index_1based"], "Unexpected event columns")
        for row in reader:
            timestamp, node = float(row["time_s"]), int(row["node_index_1based"]) - 1
            require(row["type"] == "spike", "Unexpected non-neuronal event")
            require(math.isfinite(timestamp) and 0 < timestamp <= duration and timestamp >= previous,
                    "Invalid or unordered event timestamp")
            require(0 <= node < N_NEURONS, "Invalid event neuron")
            require(timestamp > last_event_per_neuron[node], "Duplicate same-neuron event at one timestamp")
            last_event_per_neuron[node], previous = timestamp, timestamp
            per_neuron[node] += 1
            bins[event_bin(timestamp)] += 1
            if timestamp > 5.:
                late_excitatory += node < N_EXCITATORY
                late_inhibitory += node >= N_EXCITATORY
                comparison_events += timestamp <= min(duration, 30.)
    return bins, per_neuron, late_excitatory, late_inhibitory, comparison_events


def run_check(name, spec, root, provenance, published):
    configs, results = root / "configs/populations", root / "results/populations"
    raw = results / "raw"
    base_path, graph_path = configs / f"{name}_base.json", configs / f"{spec['graph']}_edges.csv"
    require(digest(base_path) == provenance["base_sha256"], "Base SHA differs from run provenance")
    require(digest(graph_path) == provenance["edges_sha256"], "Graph SHA differs from run provenance")
    for suffix, expected in provenance["outputs"].items():
        require(digest(raw / f"{name}_{suffix}") == expected, f"{name}: output SHA mismatch ({suffix})")
    base = read_json(base_path)
    require(read_json(raw / f"{name}_base.json") == base, "Exported base differs from the requested experiment")
    require(len(base["neurons"]) == len(base["synapses"]) == 1 and not base["astrocytes"] and not base["gaps"],
            "Unexpected population template structure")
    neuron, synapse = base["neurons"][0], base["synapses"][0]
    require(neuron["model"] == "izhikevich" and neuron["current"] == neuron["amplitude"] == 0., "Unexpected external drive/model")
    require(not neuron["individual_noise"] and base["noise_interval"] == .001, "Unexpected noise protocol")
    require(synapse["model"] == "tsodyks_markram" and not synapse["stdp"] and synapse["enabled"], "Unexpected synaptic protocol")
    duration = base["duration"]
    summary = read_json(raw / f"{name}_summary.json")
    require(summary["complete"] and not summary["error"], "Incomplete numerical run")
    approximately(summary["time_s"], duration, "Final time")
    bins, per_neuron, late_e, late_i, comparison_events = read_event_bins(raw / f"{name}_events.csv", duration)
    require(per_neuron == summary["neuron_spikes"], "Independent event counts differ from native summary")
    window_counts = sliding_sums(bins, 50)
    candidates = interior_peaks(window_counts)
    peaks = separated_peaks(window_counts, candidates, 50)
    burst_bins = [0] * len(bins)
    for peak in peaks:
        burst_bins[peak] = 1
    five_second_counts = sliding_sums(burst_bins, 5000)
    rates = [count / 25. for count in window_counts]
    max_rate_error = max_burst_error = 0.
    curves = results / f"{name}_characteristics.csv"
    row_count = 0
    with curves.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == ["time_s", "population_rate_50ms_hz", "burst_rate_5s_hz"], "Unexpected characteristic columns")
        for index, row in enumerate(reader):
            require(index < len(bins), "Too many characteristic samples")
            approximately(float(row["time_s"]), index / 1000., "Characteristic time", 1e-9)
            actual_rate, actual_bursts = float(row["population_rate_50ms_hz"]), float(row["burst_rate_5s_hz"])
            approximately(actual_rate, rates[index], "Population characteristic", 1e-7)
            approximately(actual_bursts, five_second_counts[index] / 5., "Burst characteristic", 1e-7)
            max_rate_error = max(max_rate_error, abs(actual_rate - rates[index]))
            max_burst_error = max(max_burst_error, abs(actual_bursts - five_second_counts[index] / 5.))
            row_count += 1
    require(row_count == len(bins), "Missing characteristic samples")
    span = duration - 5.
    require(span > 0, "Analysis needs a duration exceeding the five-second transient")
    comparison_end = min(duration, 30.)
    comparison_span = comparison_end - 5.
    require(published["comparison_window_s"] == [5., comparison_end], "Unexpected common comparison window")
    scalar = dict(duration_s=duration, spikes=sum(per_neuron), active_neurons=sum(count > 0 for count in per_neuron),
                  mean_rate_hz=(late_e + late_i) / (500 * span), excitatory_rate_hz=late_e / (400 * span),
                  inhibitory_rate_hz=late_i / (100 * span), max_population_rate_hz=max(rates),
                  population_rate_sd_hz=statistics.pstdev(rates[5001:]), bursts=len(peaks),
                  burst_rate_hz=sum(peak > 5000 for peak in peaks) / span,
                  comparison_mean_rate_hz=comparison_events / (500 * comparison_span),
                  comparison_burst_rate_hz=sum(5000 < peak <= round(comparison_end * 1000) for peak in peaks) / comparison_span,
                  local_peak_rate_hz=sum(peak > 5000 for peak in candidates) / span,
                  burst_rate_distance25_hz=sum(peak > 5000 for peak in separated_peaks(window_counts, candidates, 25)) / span,
                  burst_rate_distance100_hz=sum(peak > 5000 for peak in separated_peaks(window_counts, candidates, 100)) / span)
    for key, value in scalar.items():
        approximately(published[key], value, f"{name}: {key}")
    excursion_rate = sum(peak > 5000 for peak in excursion_peaks(window_counts)) / span
    for optional_key in ["excursion_burst_rate_hz", "burst_rate_excursion_hz"]:
        if optional_key in published:
            approximately(published[optional_key], excursion_rate, f"{name}: {optional_key}")
    if spec["role"] == "zero_noise_control":
        require(sum(per_neuron) == 0 and base["noise_sigma"] == 0., "Quiet control was not quiet")
    else:
        require(base["noise_sigma"] == 3., "Active scenario must use the specified sigma=3")
    return dict(passed=True, recomputed=scalar, excursion_burst_rate_hz=excursion_rate,
                compared_curve_rows=row_count, maximum_rate_csv_error=max_rate_error,
                maximum_burst_csv_error=max_burst_error, characteristics_sha256=digest(curves),
                events_sha256=digest(raw / f"{name}_events.csv"), executable_sha256=provenance["executable_sha256"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=HERE)
    parser.add_argument("--only", nargs="+")
    parser.add_argument("--graphs-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    results = root / "results/populations"
    output = args.output or results / ("independent_graph_checks.json" if args.graphs_only else "independent_checks.json")
    report = dict(passed=False, created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  python=platform.python_version(), script_sha256=digest(Path(__file__)),
                  mode="graphs_only" if args.graphs_only else "graphs_and_metrics", self_test=self_test(), graphs={}, runs={},
                  method="Independent stdlib CSV reader; 1-ms bins and integer sliding sums; plateau walk and sorted-neighbor peak suppression")
    try:
        manifest, graph_stats = read_json(results / "manifest.json"), read_json(results / "graphs.json")
        selected = args.only or list(manifest["scenarios"])
        require(all(name in manifest["scenarios"] for name in selected), "Unknown selected scenario")
        graph_names = sorted({manifest["scenarios"][name]["graph"] for name in selected})
        for name in graph_names:
            report["graphs"][name] = graph_check(root / f"configs/populations/{name}_edges.csv", graph_stats[name])
        if not args.graphs_only:
            provenance, metrics = read_json(results / "run_provenance.json"), read_json(results / "metrics.json")
            for name in selected:
                report["runs"][name] = run_check(name, manifest["scenarios"][name], root, provenance[name], metrics["runs"][name])
                print(f"{name}: independent metrics PASS", flush=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        print(report["error"], file=sys.stderr)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'}: {len(report['graphs'])} graphs, {len(report['runs'])} runs; {output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
