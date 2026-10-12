"""Independently replay pair STDP from the production CLI's exported spikes.

Run with Python 3.9+ (standard library only):
    python stdp_check.py --cli build/Release/network_studio_cli.exe
Add --plot to create a figure when matplotlib is installed. --self-test needs
no executable. Outputs default to the ignored verification/stdp directory.

The oracle enumerates strictly earlier spike pairs; it does NOT integrate a
neuron or repeat the C++ trace recurrence. At each event timestamp it sums both
polarities using the OLD weight, then clamps ONCE. Same-time pairs are excluded.
This checks learning against the exported events, not the accuracy of neuronal
event times or the biological adequacy of the chosen learning rule.

References:
https://neuronaldynamics.epfl.ch/online/Ch19.S2.html (19.10--19.14)
UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf, p. 8, (8)--(11):
lambda=.001, alpha=5, tau=10 ms; F+=lambda*(1-w), F-=lambda*alpha*w.
"""

import argparse
import bisect
import copy
import csv
import datetime
import hashlib
import json
import math
import os
import platform
import subprocess
import sys
import time
from pathlib import Path


TOLERANCE = 1e-10


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact(prefix, suffix):
    return Path(str(prefix) + suffix)


def pair_oracle(config, event_groups):
    """Return timestamped weights computed by direct pair enumeration."""
    synapses = config["synapses"]
    weights = [s["weight"] for s in synapses]
    past = [[] for _ in config["neurons"]]
    timeline = [(0.0, weights.copy())]
    counts = [{"ltp_pairs": 0, "ltd_pairs": 0, "excluded_same_time_pairs": 0}
              for _ in synapses]
    for timestamp, nodes in event_groups:
        for index, synapse in enumerate(synapses):
            if not (synapse["enabled"] and synapse["stdp"]):
                continue
            source, target = synapse["source"], synapse["target"]
            pre, post = source in nodes, target in nodes
            if not (pre or post):
                continue
            # Histories contain strictly earlier events. New spikes are appended
            # only after ALL synapses have been processed for this timestamp.
            positive = math.fsum(math.exp(-(timestamp - earlier) / synapse["tau_plus"])
                                 for earlier in past[source]) if post else 0.0
            negative = math.fsum(math.exp(-(timestamp - earlier) / synapse["tau_minus"])
                                 for earlier in past[target]) if pre else 0.0
            counts[index]["ltp_pairs"] += len(past[source]) if post else 0
            counts[index]["ltd_pairs"] += len(past[target]) if pre else 0
            counts[index]["excluded_same_time_pairs"] += int(pre and post)
            old = weights[index]
            rule = synapse["stdp_rule"]
            require(rule in ("additive", "multiplicative"), "Unknown STDP rule")
            plus_factor = synapse["weight_max"] - old if rule == "multiplicative" else 1.0
            minus_factor = old - synapse["weight_min"] if rule == "multiplicative" else 1.0
            delta = synapse["a_plus"] * plus_factor * positive - synapse["a_minus"] * minus_factor * negative
            weights[index] = min(synapse["weight_max"], max(synapse["weight_min"], old + delta))
        for node in nodes:
            past[node].append(timestamp)
        timeline.append((timestamp, weights.copy()))
    return timeline, counts


def self_test():
    """Analytical examples also check the oracle's signs and pair semantics."""
    synapse = dict(source=0, target=1, enabled=True, stdp=True,
                   stdp_rule="additive", weight=1., a_plus=.01, a_minus=.012,
                   tau_plus=.02, tau_minus=.02, weight_min=0., weight_max=2.)
    config = dict(neurons=[{}, {}], synapses=[synapse])
    examples = [
        ([(0., {0}), (.01, {1})], 1. + .01 * math.exp(-.5)),
        ([(0., {1}), (.01, {0})], 1. - .012 * math.exp(-.5)),
        ([(0., {0}), (.01, {0}), (.02, {1})], 1. + .01 * (math.exp(-.5) + math.exp(-1))),
        ([(0., {0}), (.01, {1}), (.02, {0, 1})],
         1. + .01 * math.exp(-.5) + .01 * math.exp(-1) - .012 * math.exp(-.5)),
        ([(0., {0, 1}), (.01, {0, 1})], 1. + (.01 - .012) * math.exp(-.5)),
    ]
    for events, expected in examples:
        timeline, _ = pair_oracle(config, events)
        require(abs(timeline[-1][1][0] - expected) < 1e-14, "Pair oracle analytical self-test failed")
    # At the lower boundary both directions must be summed before clipping.
    synapse["weight"] = 0.
    timeline, _ = pair_oracle(config, [(0., {0, 1}), (.01, {0, 1})])
    require(timeline[-1][1][0] == 0., "Oracle clipped simultaneous polarities separately")
    synapse.update(stdp_rule="multiplicative", weight=.5, a_plus=.001,
                   a_minus=.005, tau_plus=.01, tau_minus=.01, weight_max=1.)
    timeline, _ = pair_oracle(config, [(0., {0}), (.01, {1})])
    require(abs(timeline[-1][1][0] - (.5 + .0005 * math.exp(-1))) < 1e-14,
            "Multiplicative oracle analytical self-test failed")
    timeline, _ = pair_oracle(config, [(0., {0, 1}), (.01, {0, 1})])
    require(abs(timeline[-1][1][0] - (.5 + (.0005 - .0025) * math.exp(-1))) < 1e-14,
            "Multiplicative simultaneous update must use the same old weight")
    return {"passed": True, "analytical_cases": len(examples) + 3}


def read_events(path, config):
    groups = {}
    counts = [0] * len(config["neurons"])
    previous = -math.inf
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            timestamp = float(row["time_s"])
            require(math.isfinite(timestamp) and 0 < timestamp <= config["duration"], "Invalid event time")
            require(timestamp >= previous, "Events are not in chronological order")
            previous = timestamp
            require(row["type"] in ("spike", "astro_release"), "Unknown event type")
            if row["type"] != "spike":
                continue
            node = int(row["node_index_1based"]) - 1
            require(0 <= node < len(counts), "Event references an invalid neuron")
            nodes = groups.setdefault(timestamp, set())
            require(node not in nodes, "Duplicate neuron event at a single timestamp")
            nodes.add(node)
            counts[node] += 1
    return sorted(groups.items()), counts


def check_case(prefix, expected_direction, expected_config=None):
    config = read_json(artifact(prefix, "_experiment.json"))
    require(config["schema"] >= 3, "This check requires the STDP-enabled schema-3 CLI")
    require(config["noise_sigma"] == 0 and all(n["noise_sigma"] == 0 for n in config["neurons"]),
            "STDP reference scenarios must have noise disabled")
    if expected_config is not None:
        require(config == expected_config, "CLI exported configuration differs from the supplied configuration")
    groups, spike_counts = read_events(artifact(prefix, "_events.csv"), config)
    require(all(count >= 2 for count in spike_counts), "Scenarios need repeated pre and post events")
    timeline, pair_counts = pair_oracle(config, groups)
    event_times = [entry[0] for entry in timeline]
    with artifact(prefix, "_weights.csv").open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        expected_header = ["time_s"] + [f"S{i + 1}_weight" for i in range(len(config["synapses"]))]
        require(reader.fieldnames == expected_header, "Weight CSV column names differ from the synapse configuration")
        rows = [[float(row[field]) for field in expected_header] for row in reader]
    require(len(rows) >= 2 and rows[0][0] == 0 and rows[-1][0] == config["duration"],
            "Weight export must include the initial and final state")
    require(all(left[0] < right[0] for left, right in zip(rows, rows[1:])), "Weight times are not strictly increasing")
    max_error, worst_time = 0., 0.
    for row in rows:
        require(all(math.isfinite(value) for value in row), "Non-finite weight export")
        expected = timeline[bisect.bisect_right(event_times, row[0]) - 1][1]
        for index, (actual, reference) in enumerate(zip(row[1:], expected)):
            synapse = config["synapses"][index]
            if synapse["stdp"]:
                require(synapse["weight_min"] - TOLERANCE <= actual <= synapse["weight_max"] + TOLERANCE,
                        f"S{index + 1} left its learning bounds")
            error = abs(actual - reference)
            if error > max_error:
                max_error, worst_time = error, row[0]
    require(max_error <= TOLERANCE, f"Independent pair replay disagrees: max error {max_error:.12g} at {worst_time:.9g} s")
    summary = read_json(artifact(prefix, "_summary.json"))
    require(summary["complete"] and not summary["error"] and summary["time_s"] == config["duration"],
            "CLI did not complete the experiment")
    require(summary["neuron_spikes"] == spike_counts, "Event CSV and summary counts differ")
    require(summary["final_synapse_weights"] == rows[-1][1:], "Weight CSV and summary final weights differ")
    require(len(config["synapses"]) == 1, "These preset checks expect one synapse")
    initial, final = rows[0][1], rows[-1][1]
    require(initial == config["synapses"][0]["weight"], "Initial recorded weight differs from the configuration")
    require((final - initial) * expected_direction > 1e-6, "Expected net potentiation/depression was absent")
    require(pair_counts[0]["ltp_pairs"] > 1 and pair_counts[0]["ltd_pairs"] > 1,
            "The scenario did not exercise both directions and multiple pairs")
    result = dict(name=prefix.name, passed=True, rule=config["synapses"][0]["stdp_rule"],
                  duration_s=config["duration"], dt_s=config["dt"], sample_s=config["sample"],
                  spike_counts=spike_counts, initial_weights=rows[0][1:], final_weights=rows[-1][1:],
                  oracle_final_weights=timeline[-1][1], compared_rows=len(rows),
                  maximum_absolute_error=max_error, worst_error_time_s=worst_time,
                  tolerance=TOLERANCE, pair_counts=pair_counts,
                  source_sha256={suffix: sha256(artifact(prefix, suffix)) for suffix in
                                 ("_experiment.json", "_events.csv", "_weights.csv", "_summary.json")})
    plot_data = dict(name=prefix.name, rows=rows, oracle=timeline)
    return result, plot_data


def plot_results(data, destination):
    # Keep optional plotting caches beside the verification outputs; restricted
    # environments may not allow writing matplotlib's default home directory.
    os.environ.setdefault("MPLCONFIGDIR", str(destination.parent / "matplotlib-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(len(data), 1, figsize=(8.0, 7.5), constrained_layout=True)
    for axis, case in zip(axes, data):
        axis.plot([row[0] for row in case["rows"]], [row[1] for row in case["rows"]],
                  color="#17608d", linewidth=1.4, label="Production CLI")
        axis.plot([event[0] for event in case["oracle"]], [event[1][0] for event in case["oracle"]],
                  linestyle="none", marker=".", markersize=3, color="#ba501b", label="Independent pair replay")
        axis.set_title(case["name"].replace("_", " "), loc="left", fontsize=11)
        axis.set_xlabel("Time (s)")
        axis.set_ylabel("Synaptic weight")
        axis.grid(alpha=.2)
        axis.legend(frameon=False, fontsize=9)
    figure.savefig(destination, dpi=180)
    plt.close(figure)


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", type=Path, default=root / "build/Release/network_studio_cli.exe")
    parser.add_argument("--output", type=Path, default=root / "verification/stdp")
    parser.add_argument("--plot", action="store_true", help="Also generate a PNG (requires matplotlib)")
    parser.add_argument("--self-test", action="store_true", help="Only test the independent oracle; no CLI needed")
    args = parser.parse_args()
    tests = self_test()
    if args.self_test:
        print(json.dumps(tests))
        return 0
    cli, output = args.cli.resolve(), args.output.resolve()
    require(cli.is_file(), f"CLI executable not found: {cli}")
    output.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    report = dict(passed=False, created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  python=platform.python_version(), python_executable=sys.executable,
                  cli=str(cli), cli_sha256=sha256(cli), script_sha256=sha256(Path(__file__)),
                  method="Direct enumeration of strictly earlier spike pairs; chronological grouped updates; one clamp per timestamp",
                  references=["https://neuronaldynamics.epfl.ch/online/Ch19.S2.html",
                              "UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf, p.8, equations (8)-(11)"],
                  multiplicative_initial_weight_note="w0=0.5 is an explicit test choice; p.8 does not specify an initial weight",
                  self_test=tests, commands=[], cases=[])
    plots = []

    def run(*arguments):
        command = [str(cli), *map(str, arguments)]
        report["commands"].append(command)
        process = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        require(process.returncode == 0, f"CLI failed ({process.returncode}): {process.stderr.strip()} {process.stdout.strip()}")

    try:
        for preset, name, direction in ((6, "additive_pre_before_post", 1), (7, "additive_post_before_pre", -1)):
            prefix = output / name
            run("--preset", preset, "--output", prefix)
            result, plot_data = check_case(prefix, direction)
            report["cases"].append(result)
            plots.append(plot_data)
            print(f"{name}: PASS, final w={result['final_weights'][0]:.12g}, max error={result['maximum_absolute_error']:.3g}")
        manual = copy.deepcopy(read_json(artifact(output / "additive_pre_before_post", "_experiment.json")))
        manual["name"] = "STDP / PDF p.8 multiplicative pair rule"
        manual["noise_sigma"] = 0.
        for neuron in manual["neurons"]:
            neuron["individual_noise"], neuron["noise_sigma"] = False, 0.
        manual["synapses"][0].update(stdp_rule="multiplicative", weight=.5, a_plus=.001, a_minus=.005,
                                     tau_plus=.010, tau_minus=.010, weight_min=0., weight_max=1.)
        manual_path = output / "manual_multiplicative_config.json"
        write_json(manual_path, manual)
        prefix = output / "manual_multiplicative"
        run("--config", manual_path, "--output", prefix)
        result, plot_data = check_case(prefix, 1, manual)
        report["cases"].append(result)
        plots.append(plot_data)
        print(f"manual_multiplicative: PASS, final w={result['final_weights'][0]:.12g}, max error={result['maximum_absolute_error']:.3g}")
        require(sha256(cli) == report["cli_sha256"], "The CLI executable changed during verification; rerun on one build")
        if args.plot:
            destination = output / "stdp_replay.png"
            plot_results(plots, destination)
            report["figure"] = str(destination)
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        print(report["error"], file=sys.stderr)
    finally:
        report["elapsed_s"] = time.perf_counter() - started
        write_json(output / "stdp_summary.json", report)
    print(f"Summary: {output / 'stdp_summary.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
