"""Compare lab 3 screenshots with native CLI runs and exact TM event replay.

The diagnostic coarse Euler neuron is a translation, NOT a NeuroNet run.
Raw traces are reproducible and ignored by Git. Configs open in the GUI.
"""
from __future__ import annotations
import argparse
<<<<<<< ours
=======
import copy
>>>>>>> theirs
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parent
FREQUENCIES = [.1, .5, 1., 5., 10., 25., 50., 100.]
OBSERVED = [14.5, 14.5, 14.5, 17.5, 15.5, 10., 6.75, 6.75]


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_config(frequency, dt=.000025, width=.003):
    c = json.loads((STUDIO / "lab2/configs/rings/tm_n03_w1.json").read_text(encoding="utf-8"))
    period = 1 / frequency
    c.update(name=f"Lab 3 / {frequency:g} Hz / 30 x {width*1000:g} ms", duration=max(20., 6. + 5.1*period), dt=dt,
             sample=.001, noise_sigma=0., neurons=c["neurons"][:2], synapses=c["synapses"][:1])
    for i, n in enumerate(c["neurons"]):
        n.update(px=.3+.4*i, py=.5, amplitude=30. if i == 0 else 0., frequency=frequency,
                 start=6., stop=c["duration"], width=width, voltage=-70., izh_u=0., equilibrium_u=False)
    return c


def coarse_neuron(c):
    """Generalized lab1/neuronet_probe.py Euler and output timer translation.

    Initial state and 6-s silence are explicit. Quantized, right-closed pulses;
    timestamps use step END for compatibility with the replay, unlike old probe.
    Synapse is replayed analytically later, so this is not a whole-program clone.
    """
    n = c["neurons"][0]
    h, v, u, flag, timer = .5, -70., 0., False, 0.
    period_ticks = round(10000 / n["frequency"])
    start_ticks = round(n["start"] * 10000)
    width_ticks = round(n["width"] * 10000)
    events = []
    for step in range(round(c["duration"]*1000/h)):
        ticks = step * 5
        phase = (ticks-start_ticks) % period_ticks
        current = n["amplitude"] if ticks >= start_ticks and 0 < phase <= width_ticks else 0.
        dv, du = .04*v*v+5*v+140-u+current, .02*(.2*v-u)
        v, u = v+h*dv, u+h*du
        threshold = v >= 30
        if threshold:
            flag, v, u = True, -65., u+8.
        if timer:
            timer -= h
            if timer <= 0:
                flag, timer = False, 0.
        if timer == 0 and flag:
            events.append((step+1)*h/1000)
            timer = 1.
        if threshold and (not events or events[-1] != (step+1)*h/1000):
            raise AssertionError("Output timer unexpectedly suppressed a threshold")
    return events


def period_metrics(events, responses, c):
    f, start = c["neurons"][0]["frequency"], c["neurons"][0]["start"]
    # Use complete periods only, after 5 s of driving and at least 2 periods.
    first = max(2, math.ceil(5*f))
    last = math.floor((c["duration"]-start)*f+1e-8)
    if last <= first:
        raise AssertionError("Not enough full steady periods")
    peaks, counts = [], []
    for k in range(first, last):
        left, right = start+k/f, start+(k+1)/f
        rows = [r for r in responses if left <= r["time_s"] < right]
<<<<<<< ours
        # A stimulus with no spike has no response peak. Do not average a
        # fictitious zero into the measured height (notably at 100-Hz drive).
        if rows:
            peaks.append(max(r["peak"] for r in rows))
        counts.append(len(rows))
    first_events = [t-start for t in events if start <= t < start+1/f]
    return dict(steady_window_s=[start+first/f, start+last/f], periods=len(counts), responding_periods=len(peaks),
=======
        peaks.append(max((r["peak"] for r in rows), default=0.))
        counts.append(len(rows))
    first_events = [t-start for t in events if start <= t < start+1/f]
    return dict(steady_window_s=[start+first/f, start+last/f], periods=len(counts),
>>>>>>> theirs
                spikes_per_period_min=min(counts), spikes_per_period_max=max(counts),
                spikes_per_period_mean=statistics.mean(counts), spike_rate_hz=statistics.mean(counts)*f,
                peak_mean=statistics.mean(peaks), peak_min=min(peaks), peak_max=max(peaks),
                first_period_spike_times_after_stimulus_ms=[t*1000 for t in first_events])


def run_case(name, c, cli, skip, tm):
    path = HERE / "configs" / (name+".json")
    save(path, c)
    prefix = HERE / "results/raw" / name
    prefix.parent.mkdir(parents=True, exist_ok=True)
    if not skip:
        done = subprocess.run([str(cli), "--config", str(path), "--output", str(prefix)],
                              capture_output=True, text=True, check=True)
        prefix.with_name(name+"_cli.log").write_text(done.stdout+done.stderr, encoding="utf-8")
    snapshot = json.loads(prefix.with_name(name+"_experiment.json").read_text(encoding="utf-8"))
    assert snapshot == c, name+": mismatched snapshot"
    summary = json.loads(prefix.with_name(name+"_summary.json").read_text(encoding="utf-8"))
    assert summary["complete"] and not summary["error"] and abs(summary["time_s"]-c["duration"]) < 1e-9
    assert summary["final_synapse_weights"] == [1.]
    with prefix.with_name(name+"_events.csv").open(encoding="utf-8", newline="") as stream:
        events = [float(r["time_s"]) for r in csv.DictReader(stream) if r["node_index_1based"] == "1"]
    assert len(events) == summary["neuron_spikes"][0]
    responses = tm.tm_responses(events)
    trace = []
    with prefix.with_name(name+"_trace.csv").open(encoding="utf-8", newline="") as stream:
        for r in csv.DictReader(stream):
            trace.append(tuple(float(r[key]) for key in ("time_s", "S1_u", "S1_x", "S1_y")))
    references = tm.state_at(events, [r[0] for r in trace])
    errors = {key: max(abs(a[j+1]-b[key]) for a,b in zip(trace, references)) for j,key in enumerate(("u","x","y"))}
    assert max(errors.values()) < 1e-8, (name, errors)
    assert all(-1e-9 <= r[2] and r[2]+r[3] <= 1+1e-9 for r in trace)
    metrics = period_metrics(events, responses, c)
    metrics.update(name=name, dt_us=c["dt"]*1e6, rows=len(trace), exact_tm_max_abs_errors=errors,
                   config_sha256=digest(path), events_sha256=digest(prefix.with_name(name+"_events.csv")),
                   trace_sha256=digest(prefix.with_name(name+"_trace.csv")))
    return metrics, events, responses


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--python-deps", type=Path)
    p.add_argument("--cli", type=Path, default=STUDIO / "build/Release/network_studio_cli.exe")
    p.add_argument("--skip-run", action="store_true")
    a = p.parse_args()
<<<<<<< ours
    if a.skip_run:
        previous = json.loads((HERE / "results/comparison.json").read_text(encoding="utf-8"))
        assert previous["cli_sha256"] == digest(a.cli), "Cached raw runs used a different executable"
        cached = [r[k] for r in previous["cases"] for k in ("native", "fine")]+[previous["single_spike_control"]]
        for r in cached:
            for kind in ("events", "trace"):
                assert r[kind+"_sha256"] == digest(HERE / "results/raw" / (r["name"]+"_"+kind+".csv")), "Cached CSV hash mismatch"
    if a.python_deps:
        sys.path.insert(0, str(a.python_deps.resolve()))
    import reference_tm as tm
    print(tm.self_test(), flush=True)
=======
    if a.python_deps:
        sys.path.insert(0, str(a.python_deps.resolve()))
    import reference_tm as tm
>>>>>>> theirs
    os.environ.setdefault("MPLCONFIGDIR", str(HERE / "results/raw/mpl"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
<<<<<<< ours
    report_path = STUDIO.parents[2] / "labs/lab3_report/Отчёт_ЛР3.md"
    result = dict(description="Native RK4 versus screenshot estimates; coarse Euler is diagnostic translation only",
                  cli_sha256=digest(a.cli), source_report=str(STUDIO.parents[2] / "labs/lab3_report/Отчёт_ЛР3.md"),
                  source_report_sha256=digest(report_path),
                  code_sha256={p.name:digest(p) for p in (Path(__file__), HERE / "reference_tm.py", STUDIO / "network.cpp")},
                  screenshot_sha256={p.name:digest(p) for p in sorted((report_path.parent / "images").glob("task1*.png"))},
=======
    result = dict(description="Native RK4 versus screenshot estimates; coarse Euler is diagnostic translation only",
                  cli_sha256=digest(a.cli), source_report=str(STUDIO.parents[1] / "labs/lab3_report/Отчёт_ЛР3.md"),
>>>>>>> theirs
                  cases=[], reference_frequency_curve=[])
    all_data = []
    for f, observed in zip(FREQUENCIES, OBSERVED):
        name = f"f{f:g}".replace(".","p")
        c = make_config(f)
        native, events, responses = run_case(name, c, a.cli, a.skip_run, tm)
        fine, _, _ = run_case(name+"_fine", make_config(f, dt=.0000125), a.cli, a.skip_run, tm)
        coarse_events = coarse_neuron(c)
        coarse_responses = tm.tm_responses(coarse_events)
        coarse = period_metrics(coarse_events, coarse_responses, c)
<<<<<<< ours
        coarse_discrete = period_metrics(coarse_events, tm.euler_tm_responses(coarse_events), c)
        row = dict(stimulus_frequency_hz=f, observed_peak_approx=observed, native=native, fine=fine,
                   coarse_euler_neuron_exact_tm=coarse,
                   coarse_euler_neuron_euler_tm=coarse_discrete,
                   fine_peak_change_percent=100*(fine["peak_mean"]-native["peak_mean"])/native["peak_mean"])
        result["cases"].append(row)
        regular = tm.regular_response(f)
        result["reference_frequency_curve"].append(dict(spike_frequency_hz=f, peak=regular["peak"], jump=regular["jump"]))
        all_data.append((f, c, events, responses, coarse_events, coarse_responses))
        print(f"{f:g}Hz: native peak={native['peak_mean']:.6f}, spikes/period={native['spikes_per_period_mean']:.4g}; translated Euler peak={coarse_discrete['peak_mean']:.6f}, spikes/period={coarse['spikes_per_period_mean']:.4g}", flush=True)
=======
        row = dict(stimulus_frequency_hz=f, observed_peak_approx=observed, native=native, fine=fine,
                   coarse_euler_neuron_exact_tm=coarse,
                   fine_peak_change_percent=100*(fine["peak_mean"]-native["peak_mean"])/native["peak_mean"])
        result["cases"].append(row)
        regular = tm.tm_responses([6+k/f for k in range(max(20, math.ceil(20*f)))])
        result["reference_frequency_curve"].append(dict(spike_frequency_hz=f, peak=regular[-1]["peak"], jump=regular[-1]["jump"]))
        all_data.append((f, c, events, responses, coarse_events, coarse_responses))
        print(f"{f:g}Hz: native peak={native['peak_mean']:.6f}, spikes/period={native['spikes_per_period_mean']:.4g}; translated Euler peak={coarse['peak_mean']:.6f}, spikes/period={coarse['spikes_per_period_mean']:.4g}", flush=True)
>>>>>>> theirs
    control, control_events, control_responses = run_case("single_spike_control", make_config(.1, width=.001), a.cli, a.skip_run, tm)
    assert control["spikes_per_period_min"] == control["spikes_per_period_max"] == 1
    assert abs(control_responses[0]["peak"]-10) < 1e-12
    result["single_spike_control"] = control
    result["single_spike_control"]["first_peak"] = control_responses[0]["peak"]
    result["total_native_runs"] = 17
    result["checked_trace_rows"] = sum(r[k]["rows"] for r in result["cases"] for k in ("native","fine")) + control["rows"]
<<<<<<< ours
    fixture = json.loads((HERE / "neuronet_snapshot_reference.json").read_text(encoding="utf-8"))
    assert fixture["first_observed_response"]["raw_output"] == control_responses[0]["peak"] == 10.
    assert fixture["pre_spike_reference"]["raw_x"] == 20.
    assert fixture["normalization"]["maximum_resource_sum_error"] < 4e-5
    residuals = {key:max(abs(row["raw_field_residuals"][key]) for row in fixture["explicit_euler_decay_residuals"]) for key in ("Output", "parameter_x", "parameter_z", "parameter_u")}
    assert residuals["Output"] < 4e-6 and residuals["parameter_x"] < 6e-5 and residuals["parameter_z"] < 3e-6 and residuals["parameter_u"] < 1e-6
    result["original_snapshot_checks"] = dict(first_output=10., euler_decay_max_raw_residuals=residuals,
        fixture_sha256=digest(HERE / "neuronet_snapshot_reference.json"), records=len(fixture["records"]))
=======
>>>>>>> theirs
    save(HERE / "results/comparison.json", result)
    figures = HERE / "results/figures"
    figures.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(10,5.7), layout="constrained")
    ax.plot(FREQUENCIES, OBSERVED,"o-",label="NeuroNet: оценки по вашим скриншотам",color="#555555")
    ax.plot(FREQUENCIES,[r["native"]["peak_mean"] for r in result["cases"]],"s-",label="Конфигуратор: RK4, 0,025 мс",color="#2069a4")
<<<<<<< ours
    ax.plot(FREQUENCIES,[r["coarse_euler_neuron_euler_tm"]["peak_mean"] for r in result["cases"]],"^--",label="Диагностика: Euler 0,5 мс (перевод алгоритма)",color="#cc7a22")
=======
    ax.plot(FREQUENCIES,[r["coarse_euler_neuron_exact_tm"]["peak_mean"] for r in result["cases"]],"^--",label="Диагностика: Euler 0,5 мс + точный TM",color="#cc7a22")
>>>>>>> theirs
    ax.plot(FREQUENCIES,[r["peak"] for r in result["reference_frequency_curve"]],":",label="Точный TM: один заданный спайк на период",color="#448844")
    ax.set_xscale("log")
    ax.set_xticks(FREQUENCIES,[str(f).removesuffix(".0").replace(".",",") for f in FREQUENCIES])
    ax.set(xlabel="Частота стимулов, Гц (зелёная линия: частота заданных спайков)",ylabel="Вершина Output относительно нуля, у. е.",ylim=(0,21),title="Лабораторная 3: стимулы 30 × 3 мс и синаптический выход")
    ax.grid(alpha=.2)
    ax.legend(fontsize=9, loc="lower left")
    fig.savefig(figures / "frequency_comparison.png", dpi=170)
    plt.close(fig)
    fig, axs = plt.subplots(1,2,figsize=(11,4.5),layout="constrained")
    for ax, item in zip(axs, (all_data[0], all_data[4])):
        f,c,events,responses,coarse_events,coarse_responses = item
        # A late complete period; show first 20 ms at low frequency, 100 ms at 10 Hz.
        start = c["neurons"][0]["start"] + (2 if f==.1 else 100)/f
        span = .02 if f==.1 else .1
        times = [start+k*.00005 for k in range(round(span/.00005)+1)]
        for ev,label,color in ((events,"RK4 0,025 мс","#2069a4"),(coarse_events,"Euler 0,5 мс + точный TM","#cc7a22")):
            values = tm.state_at(ev,times)
            ax.plot([(t-start)*1000 for t in times],[r["output"] for r in values],label=label,color=color)
            for t in ev:
                if start<=t<=start+span:
                    ax.axvline((t-start)*1000,color=color,alpha=.28,ls=":")
        ax.axvspan(0,3,color="gray",alpha=.15)
        ax.set(xlabel="Время от начала стимула, мс",ylabel="Output, у. е.",ylim=(0,21),title=f"Стимуляция {f:g} Гц")
        ax.grid(alpha=.2)
    axs[0].legend(fontsize=8)
    fig.suptitle("Различия входных спайков меняют ответ одного и того же синапса")
    fig.savefig(figures / "pulse_comparison.png",dpi=170)
    plt.close(fig)
    print(f"Verified {result['total_native_runs']} native runs, {result['checked_trace_rows']} trace rows.",flush=True)


if __name__ == "__main__":
    main()
