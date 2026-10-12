"""Lab 2 rings: all dynamics are computed by the native network_studio_cli.

Primary cases: N=3..10 at w=1, then N=10 at w=0.7,0.5,0.2,0.
One 30-unit, 1-ms pulse replaces manual Fire!; there is no repeated drive.
Additional cases halve dt, remove STP at unchanged gain, and add sigma=3 noise.
"""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parent
sys.path.insert(0, str(STUDIO / "lab1"))
from config_compat import canonical_config
CONFIGS = HERE / "configs" / "rings"
RESULTS = HERE / "results" / "rings"
RAW = RESULTS / "raw"
FIGURES = HERE / "results" / "figures"
WEIGHTS = [1., .7, .5, .2, 0.]
OUTPUT_SUFFIXES = ("_experiment.json", "_summary.json", "_events.csv", "_trace.csv", "_weights.csv", "_cli.log")


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative(path):
    return path.relative_to(HERE).as_posix()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def tag(weight):
    return f"{weight:g}".replace(".", "p")


def load_template(cli):
    with tempfile.TemporaryDirectory(dir=RAW, prefix="template_") as directory:
        path = Path(directory) / "preset.json"
        subprocess.run([str(cli), "--preset", "5", "--save", str(path)],
                       check=True, capture_output=True, text=True)
        template = json.loads(path.read_text(encoding="utf-8-sig"))
    require(template["schema"] in (3, 4), "A CLI with schema 3 or 4 is required.")
    return template


def make_config(template, n, weight, *, dt=.000025, synapse_model="tsodyks_markram", noise=0.):
    config = copy.deepcopy(template)
    config.update(schema=template["schema"], name=f"Lab 2 / ring N={n}, w={weight:g}, {synapse_model}",
                  dt=dt, sample=.001, duration=5., noise_sigma=noise, noise_interval=.001,
                  seed=42, neurons=[], synapses=[], astrocytes=[], gaps=[])
    for index in range(n):
        neuron = copy.deepcopy(template["neurons"][0])
        angle = 2 * math.pi * index / n - math.pi / 2
        neuron.update(name=f"N{index+1}", px=.5 + .37 * math.cos(angle),
                      py=.5 + .37 * math.sin(angle), voltage=-65., current=0.,
                      amplitude=30. if index == 0 else 0., frequency=1.,
                      start=.1, stop=.101, width=.001,
                      model="izhikevich", izh_a=.02, izh_b=.2, izh_c=-65., izh_d=8.,
                      izh_peak=30., izh_u=-13., equilibrium_u=True,
                      individual_noise=False, noise_sigma=0.)
        config["neurons"].append(neuron)
        synapse = copy.deepcopy(template["synapses"][0])
        synapse.update(source=index, target=(index+1) % n, astro=-1,
                       model=synapse_model, weight=weight, gain=20., release=.5,
                       tau_syn=.01, tau_d=.05, tau_f=1., enabled=True, stdp=False)
        config["synapses"].append(synapse)
    return config


def prepare(template, baseline_only):
    cases = {}
    for n in range(3, 11):
        cases[f"tm_n{n:02}_w1"] = ("primary", make_config(template, n, 1.))
    for weight in WEIGHTS[1:]:
        cases[f"tm_n10_w{tag(weight)}"] = ("primary", make_config(template, 10, weight))
    if not baseline_only:
        for name, (_, config) in list(cases.items()):
            fine = copy.deepcopy(config)
            fine["dt"] = .0000125
            cases[name + "_dt12p5us"] = ("convergence", fine)
        for n in (3, 4, 10):
            for weight in WEIGHTS:
                cases[f"exp_n{n:02}_w{tag(weight)}"] = (
                    "no_stp_control", make_config(template, n, weight, synapse_model="exponential_current"))
        for n in (3, 4, 10):
            cases[f"tm_n{n:02}_w1_noise3"] = ("noise_control", make_config(template, n, 1., noise=3.))
        cases["exp_n10_w0p7_dt12p5us"] = ("convergence_control", make_config(
            template, 10, .7, dt=.0000125, synapse_model="exponential_current"))
    for name, (_, config) in cases.items():
        write_json(CONFIGS / (name + ".json"), config)
    return cases


def analyze(name, group, config):
    prefix = RAW / name
    snapshot = json.loads(Path(str(prefix) + "_experiment.json").read_text(encoding="utf-8"))
    require(canonical_config(snapshot) == canonical_config(config), f"{name}: configuration snapshot mismatch")
    summary = json.loads(Path(str(prefix) + "_summary.json").read_text(encoding="utf-8"))
    require(summary["complete"] and not summary["error"], f"{name}: incomplete simulation: {summary}")
    require(abs(summary["time_s"] - config["duration"]) < 1e-10, f"{name}: wrong end time")
    require(summary["steps"] == round(config["duration"] / config["dt"]), f"{name}: wrong step count")
    n = len(config["neurons"])
    weight = config["synapses"][0]["weight"]
    require(summary["final_synapse_weights"] == [weight] * n, f"{name}: static weights changed")
    require(summary["astrocyte_releases"] == [], f"{name}: unexpected astrocytes")
    events, per_node = [], [[] for _ in range(n)]
    with Path(str(prefix) + "_events.csv").open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            t, node = float(row["time_s"]), int(row["node_index_1based"])
            require(row["type"] == "spike" and 1 <= node <= n and math.isfinite(t) and 0 <= t <= 5,
                    f"{name}: invalid event")
            require(not events or t >= events[-1][0], f"{name}: events are not sorted")
            events.append((t, node))
            per_node[node-1].append(t)
    counts = [len(values) for values in per_node]
    require(counts == summary["neuron_spikes"], f"{name}: event/summary counts disagree")
    rows = 0
    with Path(str(prefix) + "_trace.csv").open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader)
        for row in reader:
            values = [float(x) for x in row]
            require(len(values) == len(header) and all(math.isfinite(x) for x in values), f"{name}: nonfinite trace")
            require(abs(values[0] - rows * config["sample"]) < 1e-9, f"{name}: trace time grid mismatch")
            rows += 1
    require(rows == round(config["duration"] / config["sample"]) + 1, f"{name}: incomplete trace")
    weight_rows = 0
    with Path(str(prefix) + "_weights.csv").open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        next(reader)
        for row in reader:
            values = [float(x) for x in row]
            require(len(values) == n + 1 and values[1:] == [weight] * n,
                    f"{name}: invalid weight history")
            require(abs(values[0] - weight_rows * config["sample"]) < 1e-9, f"{name}: weight time grid mismatch")
            weight_rows += 1
    require(weight_rows == rows, f"{name}: weights/trace misaligned")

    def order_fraction(sequence):
        pairs = list(zip(sequence, sequence[1:]))
        return (sum(t2 > t1 and node2 == node1 % n + 1 for (t1, node1), (t2, node2) in pairs) / len(pairs)) if pairs else None

    late_events = [(t, node) for t, node in events if 4. <= t < 5.]
    late_counts = [sum(4. <= t < 5. for t in values) for values in per_node]
    after_input = [sum(t > .101 for t in values) for values in per_node]
    late_order = order_fraction(late_events)
    all_order = order_fraction(events)
    last = events[-1][0] if events else None
    sustained = (all(count >= 2 for count in late_counts) and late_order is not None and late_order >= .95
                 and last >= 4.9 and all(after_input))
    n1_late = [t for t in per_node[0] if 4. <= t < 5.]
    isi = [b-a for a, b in zip(n1_late, n1_late[1:])]
    metrics = {
        "name": name, "group": group, "n": n, "weight": weight,
        "model": config["synapses"][0]["model"], "dt_us": config["dt"] * 1e6,
        "noise_sigma": config["noise_sigma"], "config": relative(CONFIGS / (name + ".json")),
        "counts": counts, "after_input_counts": after_input, "late_4_to_5s_counts": late_counts,
        "first_spike_s": [values[0] if values else None for values in per_node],
        "last_spike_by_node_s": [values[-1] if values else None for values in per_node],
        "last_spike_s": last, "ordered_transition_fraction": all_order,
        "late_ordered_transition_fraction": late_order, "all_nodes_fired_after_input": all(after_input),
        "sustained_circulation_to_5s": sustained, "n1_late_rate_hz": float(late_counts[0]),
        "n1_late_mean_isi_ms": 1000 * statistics.mean(isi) if sustained and isi else None,
        "n1_late_isi_rate_hz": 1 / statistics.mean(isi) if sustained and isi else None,
        "n1_late_isi_cv": statistics.pstdev(isi) / statistics.mean(isi) if sustained and isi else None,
        "trace_rows": rows,
    }
    return metrics, events, per_node


def return_diagnostics(name, metrics, per_node):
    """Recorded N1 state and incoming edge during the first return from N10."""
    rows = []
    active_column = "S10_y" if metrics["model"] == "tsodyks_markram" else "S10_s"
    with (RAW / (name + "_trace.csv")).open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            t = float(row["time_s"])
            if t > .25:
                break
            rows.append({"time_s": t, "V_mV": float(row["N1_V_mV"]), "u": float(row["N1_u"]),
                         "synapse_u": float(row["S10_u"]), "synapse_x": float(row["S10_x"]),
                         "synapse_active": float(row[active_column]),
                         "incoming_I": 20 * metrics["weight"] * float(row[active_column])})
    first_return = per_node[-1][0]
    snapshots = {"before_external_pulse": rows[99],
                 "after_N1_first_spike": rows[math.ceil(per_node[0][0] / .001)],
                 "last_sample_before_first_return": rows[math.floor(first_return / .001)],
                 "first_sample_after_first_return": rows[math.ceil(first_return / .001)],
                 "five_ms_after_return": rows[math.ceil((first_return + .005) / .001)],
                 "at_200ms": rows[200]}
    window = [row for row in rows if first_return <= row["time_s"] <= first_return + .020]
    return {"case": name, "first_return_event_s": first_return,
            "next_N1_spike_s": next((t for t in per_node[0] if t > first_return), None),
            "peak_recorded_V_first_20ms_after_return_mV": max(row["V_mV"] for row in window),
            "state_sample_interval_ms": 1,
            "state_sampling_note": "Recorded post-reset samples; spike times come from the separate native event log.",
            "snapshots": snapshots}, rows


def plot_results(metrics, event_sets):
    os.environ.setdefault("MPLCONFIGDIR", str(RAW / "matplotlib_cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9, "axes.spines.top": False,
                         "axes.spines.right": False, "savefig.dpi": 160})
    raster_sets = [
        ("rings_sizes.png", [f"tm_n{n:02}_w1" for n in range(3, 11)], "Кольцо RS: TM, g=20, w=1; один запуск N1"),
        ("rings_weights.png", [f"tm_n10_w{tag(w)}" for w in WEIGHTS], "Кольцо из 10 RS: TM, g=20; влияние веса связи"),
    ]
    controls = ["tm_n10_w1", "exp_n10_w1", "exp_n10_w0p7", "tm_n10_w1_noise3"]
    if all(name in metrics for name in controls):
        raster_sets.append(("rings_controls.png", controls, "Контроли: кольцо из 10 RS, g=20; один запуск N1"))
    for filename, names, title in raster_sets:
        fig, axes = plt.subplots(len(names), 2, figsize=(12, len(names) * 1.45), squeeze=False,
                                 constrained_layout=True)
        fig.suptitle(title + "\nСлева — запуск, справа — последняя секунда; STDP выключена", fontsize=12)
        for row, name in enumerate(names):
            result = metrics[name]
            for column, limits in enumerate(((.09, .18), (4., 5.))):
                ax = axes[row, column]
                points = [(t, node) for t, node in event_sets[name] if limits[0] <= t <= limits[1]]
                if points:
                    ax.scatter([t for t, _ in points], [node for _, node in points], s=12, marker="|", color="#157d8b")
                ax.set_xlim(*limits)
                ax.set_ylim(.5, result["n"] + .5)
                ax.set_yticks([1, result["n"]])
                ax.grid(alpha=.18)
                label = f"N={result['n']}, w={result['weight']:g}"
                if filename == "rings_controls.png":
                    label = f"{'TM' if name.startswith('tm') else 'Без STP'}, w={result['weight']:g}\nσ={result['noise_sigma']:g}"
                ax.set_ylabel(label)
                if column == 0:
                    ax.axvspan(.1, .101, color="#e5a532", alpha=.5)
                else:
                    state = "циркуляция до 5 с" if result["sustained_circulation_to_5s"] else f"последний спайк {result['last_spike_s']:.3f} с"
                    ax.text(.98, .90, state, ha="right", va="top", transform=ax.transAxes, fontsize=8)
            if row == len(names)-1:
                axes[row, 0].set_xlabel("Время, с")
                axes[row, 1].set_xlabel("Время, с")
        fig.savefig(FIGURES / filename)
        plt.close(fig)
    fig, ax = plt.subplots(figsize=(8, 4.8), constrained_layout=True)
    weights = sorted(WEIGHTS)
    for prefix, label, color in (("tm", "TM / STP включена", "#157d8b"), ("exp", "Без STP: экспоненциальный ток", "#bc6436")):
        if not all(f"{prefix}_n10_w{tag(w)}" in metrics for w in weights):
            continue
        selected = [metrics[f"{prefix}_n10_w{tag(w)}"] for w in weights]
        ax.plot(weights, [m["n1_late_rate_hz"] for m in selected], "o-", label=label, color=color)
        for w, result in zip(weights, selected):
            if not result["sustained_circulation_to_5s"]:
                ax.plot(w, result["n1_late_rate_hz"], "x", ms=9, color="black")
    ax.set(xlabel="Вес всех связей w", ylabel="Частота N1 на [4,5) с, Гц", xticks=weights,
           title="10 нейронов, g=20; один запускающий импульс\n× — критерий устойчивой упорядоченной циркуляции не выполнен")
    ax.grid(alpha=.2)
    ax.legend()
    fig.savefig(FIGURES / "rings_frequency_vs_weight.png")
    plt.close(fig)
    names = [name for name in ("tm_n10_w1", "exp_n10_w1", "exp_n10_w0p7") if name in metrics]
    fig, axes = plt.subplots(3, len(names), figsize=(4 * len(names), 8), squeeze=False, constrained_layout=True)
    for col, name in enumerate(names):
        n1 = [t for t, node in event_sets[name] if node == 1]
        n10 = [t for t, node in event_sets[name] if node == 10]
        diagnostic, rows = return_diagnostics(name, metrics[name], [n1] + [[]] * 8 + [n10])
        for row_index, (field, label, color) in enumerate((("V_mV", "N1: V, мВ", "#157d8b"),
                                                         ("u", "N1: восстановление u", "#845d94"),
                                                         ("incoming_I", "Вход N10 → N1, ед. модели", "#bc6436"))):
            ax = axes[row_index, col]
            ax.plot([row["time_s"] for row in rows], [row[field] for row in rows], color=color, lw=1.3)
            ax.axvspan(.1, .101, color="#e5a532", alpha=.5)
            ax.axvline(diagnostic["first_return_event_s"], color="grey", ls="--", lw=1)
            ax.set(xlim=(.09, .21), ylabel=label)
            ax.grid(alpha=.2)
            if row_index == 0:
                peaks = [t for t in n1 if .09 <= t <= .21]
                ax.scatter(peaks, [30] * len(peaks), s=12, marker="|", color=color)
                ax.set_title(f"{'TM' if name.startswith('tm') else 'Без STP'}, w={metrics[name]['weight']:g}")
            if row_index == 2:
                ax.set_xlabel("Время, с")
    fig.suptitle("Первый возврат активности к N1 в кольце из 10 нейронов\nПунктир — первый спайк N10; маркеры V=30 — события, запись состояний через 1 мс", fontsize=11)
    fig.savefig(FIGURES / "rings_first_return.png")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=STUDIO / "build" / "Release" / "network_studio_cli.exe")
    parser.add_argument("--python-deps", type=Path, help="Optional directory containing matplotlib and numpy")
    parser.add_argument("--baseline-only", action="store_true", help="Run only the 12 primary TM cases")
    parser.add_argument("--skip-run", action="store_true", help="Validate and plot existing outputs instead of running simulations")
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    cli = args.cli.resolve()
    require(cli.is_file(), "Build network_studio_cli first or pass --cli.")
    cli_digest = sha256(cli)
    if args.skip_run:
        old_provenance = json.loads((RESULTS / "provenance.json").read_text(encoding="utf-8"))
        require(old_provenance["cli_sha256"] == cli_digest, "Existing runs used another CLI build; rerun simulations.")
    RAW.mkdir(parents=True, exist_ok=True)
    (RAW / ".gitignore").write_text("*\n!.gitignore\n", encoding="utf-8")
    template = load_template(cli)
    cases = prepare(template, args.baseline_only)
    metrics, event_sets, node_sets, runs = {}, {}, {}, {}
    for name, (group, config) in cases.items():
        config_path, prefix = CONFIGS / (name + ".json"), RAW / name
        begin = time.perf_counter()
        if not args.skip_run:
            completed = subprocess.run([str(cli), "--config", str(config_path), "--output", str(prefix)],
                                       check=True, capture_output=True, text=True)
            Path(str(prefix) + "_cli.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
        result, events, per_node = analyze(name, group, config)
        metrics[name], event_sets[name], node_sets[name] = result, events, per_node
        runs[name] = {"config": relative(config_path), "config_sha256": sha256(config_path),
                      "files": {relative(Path(str(prefix) + suffix)): sha256(Path(str(prefix) + suffix)) for suffix in OUTPUT_SUFFIXES}}
        print(f"{name}: counts={result['counts']}, late={result['late_4_to_5s_counts']}, "
              f"ordered={result['ordered_transition_fraction']}, sustained={result['sustained_circulation_to_5s']}, "
              f"last={result['last_spike_s']}, {time.perf_counter()-begin:.2f}s", flush=True)
    convergence = []
    for name, (group, _) in cases.items():
        if group not in ("primary", "no_stp_control") or name + "_dt12p5us" not in metrics:
            continue
        fine_name = name + "_dt12p5us"
        same_counts = metrics[name]["counts"] == metrics[fine_name]["counts"]
        drifts = [abs(a-b) for coarse, fine in zip(node_sets[name], node_sets[fine_name])
                  for a, b in zip(coarse, fine)] if same_counts else []
        convergence.append({"case": name, "fine_case": fine_name, "same_counts": same_counts,
                            "same_circulation_classification": metrics[name]["sustained_circulation_to_5s"] == metrics[fine_name]["sustained_circulation_to_5s"],
                            "max_paired_event_difference_ms": 1000 * max(drifts) if drifts else None,
                            "n1_late_rate_difference_hz": metrics[fine_name]["n1_late_rate_hz"] - metrics[name]["n1_late_rate_hz"]})
    diagnostics = [return_diagnostics(name, metrics[name], node_sets[name])[0]
                   for name in ("tm_n10_w1", "exp_n10_w1", "exp_n10_w0p7") if name in metrics]
    require(sha256(cli) == cli_digest, "CLI executable changed during this run.")
    write_json(RESULTS / "metrics.json", {
        "scope": "Native CLI; one 30 x 1 ms pulse at N1, t=0.1 s; no periodic external drive",
        "primary_synapse": "Tsodyks-Markram: gain=20, U=.5, tau_syn=10ms, tau_rec=50ms, tau_fac=1000ms; STDP off",
        "circulation_criterion": {"all_nodes_after_input": True, "minimum_events_per_node_in_last_second": 2,
                                  "minimum_ordered_neighbor_fraction_in_last_second": .95, "last_event_at_or_after_s": 4.9,
                                  "limitation": "Finite 5-second observation, not proof of indefinite stability; noise controls can fire spontaneously."},
        "no_stp_control": "Exponential current with unchanged gain=20, s+=1 per event (full release); not equivalent to the first TM pulse with release .5",
        "noise_control": "Global sigma=3 held for 1ms, seed42; additional spontaneous firing is possible",
        "delay_model": "No explicit conduction delay. Propagation latency emerges from synaptic current and neuron response. No conversion of NeuroNet Sp.Velocity was applied; its units were not established.",
        "cases": metrics, "convergence": convergence, "first_return_diagnostics": diagnostics,
    })
    write_json(RESULTS / "provenance.json", {
        "cli_filename": cli.name, "cli_sha256": cli_digest, "script_sha256": sha256(Path(__file__)),
        "python_version": platform.python_version(), "raw_outputs_are_ignored": True,
        "core_sources_sha256": {name: sha256(STUDIO / name) for name in ("network.cpp", "network.h", "io.cpp")},
        "all_config_snapshots_exact": True, "all_runs_complete": True, "all_traces_finite": True,
        "all_event_counts_match_summaries": True, "all_static_weights_unchanged": True, "runs": runs,
    })
    if not args.no_plots:
        plot_results(metrics, event_sets)
    print(f"Validated {len(metrics)} cases; metrics: {RESULTS / 'metrics.json'}")


if __name__ == "__main__":
    main()
