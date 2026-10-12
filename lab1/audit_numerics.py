"""Independent numerical audit of a single I=30, 3 ms Izhikevich stimulus.

The reference uses scipy event location and splits at current discontinuities.
It does not import the production simulator or reproduce.py. All time units in
the reference are milliseconds; conversion to seconds happens only at the CLI.
This suite concerns an isolated pulse after rest, not a steady periodic train.
Optional spike holds are retained as rejected diagnostic hypotheses: the original
binary continues integrating v/u during its display/event-counter interval.
See results/neuronet_comparison/README.md for that separate executable audit.
Run after building: python lab1/audit_numerics.py
"""
from __future__ import annotations

import argparse
import copy
import csv
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess
from config_compat import canonical_config

import numpy as np
import scipy
from scipy.integrate import solve_ivp

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parent
OUT = HERE / "results" / "audit_numerics"
RAW = STUDIO / "verification" / "audit_numerics"


@dataclass(frozen=True)
class Case:
    a: float = .02
    b: float = .2
    c: float = -65.
    d: float = 8.
    peak: float = 30.
    v0: float = -65.
    u0: float = -13.
    onset: float = 100.
    width: float = 3.
    amplitude: float = 30.
    after: float = 100.

    @property
    def end(self):
        return self.onset + self.after


def rhs(y, current, p):
    v, u = y
    return np.array([.04*v*v + 5*v + 140 - u + current, p.a*(p.b*v-u)])


def event_record(t, before, after, p, reset_time=None):
    return dict(time_ms=float(t), latency_ms=float(t-p.onset),
                v_before=float(before[0]), u_before=float(before[1]),
                v_after=float(after[0]), u_after=float(after[1]),
                reset_time_ms=float(t if reset_time is None else reset_time))


def exact(p=Case(), method="DOP853", hold="none", hold_ms=1.):
    """Piecewise-constant current, terminal upward v=peak events, exact reset.

    hold=none is the published hybrid ODE. Other holds explicitly CHANGE it.
    peak_* postpones reset; reset_* resets immediately. *_frozen freezes u;
    *_recovery evolves u analytically at the held voltage. Current duration is
    not extended while the neuron is held.
    """
    t = 0.
    y = np.array([p.v0, p.u0], dtype=float)
    boundaries = sorted(set([0., p.onset, p.onset+p.width, p.end]))
    events, trace = [], [(t, *y)]
    checkpoints = {}

    def crossing(_t, state):
        return state[0]-p.peak
    crossing.terminal = True
    crossing.direction = 1

    def checkpoint(time, state):
        if abs(time-p.onset) < 1e-9:
            checkpoints["onset"] = [float(x) for x in state]
        if abs(time-(p.onset+p.width)) < 1e-9:
            checkpoints["offset"] = [float(x) for x in state]

    checkpoint(t, y)
    while t < p.end-1e-10:
        boundary = next(x for x in boundaries if x > t+1e-10)
        current = p.amplitude if p.onset <= t < p.onset+p.width else 0.
        solution = solve_ivp(lambda _t, state: rhs(state, current, p), (t, boundary), y,
                             method=method, rtol=1e-11, atol=1e-12,
                             max_step=.5, events=crossing)
        if not solution.success:
            raise RuntimeError(solution.message)
        trace.extend(zip(solution.t[1:].tolist(), *solution.y[:, 1:].tolist()))
        t = float(solution.t[-1])
        y = solution.y[:, -1].copy()
        checkpoint(t, y)
        if not len(solution.t_events[0]):
            continue
        before = y.copy()
        reset_state = np.array([p.c, y[1]+p.d])
        if hold == "none":
            y = reset_state
            events.append(event_record(t, before, y, p))
            trace.append((t, *y))
            continue
        if hold not in {"peak_frozen", "peak_recovery", "reset_frozen", "reset_recovery"}:
            raise ValueError(hold)
        delayed_reset = hold.startswith("peak")
        y = before.copy() if delayed_reset else reset_state
        hold_start, hold_initial = t, y.copy()
        hold_end = min(t+hold_ms, p.end)
        trace.append((t, *y))
        for point in sorted(set([x for x in boundaries if t < x < hold_end]+[hold_end])):
            y = hold_initial.copy()
            if hold.endswith("recovery"):
                target = p.b*hold_initial[0]
                y[1] = target+(hold_initial[1]-target)*math.exp(-p.a*(point-hold_start))
            checkpoint(point, y)
            trace.append((point, *y))
        t = hold_end
        if delayed_reset:
            y[0] = p.c
            y[1] += p.d
            reset_state = y.copy()
        events.append(event_record(hold_start, before, reset_state, p,
                                   reset_time=t if delayed_reset else hold_start))
        trace.append((t, *y))
    return dict(method=method, hold=hold, step_ms=None, parameters=asdict(p),
                events=events, checkpoints=checkpoints, final=y.tolist(),
                dose_current_ms=p.amplitude*p.width, trace=trace)


def fixed(p=Case(), step=.5, method="euler", endpoints="left_closed"):
    """Explicit Euler or historical two half-v updates, then one full u update.

    Spikes are registered/reset at step end (equivalent to detection before
    the following step). No event interpolation or refractory interval is used.
    Euler evaluates both derivatives at the old state. semi_implicit evaluates
    u using the new v. historical_old_u is only an update-order diagnostic.
    """
    y = np.array([p.v0, p.u0], dtype=float)
    events, trace, checkpoints = [], [(0., *y)], {}
    dose = 0.
    for index in range(math.ceil(p.end/step)):
        t = index*step
        dt = min(step, p.end-t)
        right = p.onset+p.width
        at_start, at_end = abs(t-p.onset)<1e-9, abs(t-right)<1e-9
        inside = t > p.onset+1e-9 and t < right-1e-9
        active = inside or (at_start and endpoints in {"left_closed", "both_closed"}) or (
            at_end and endpoints in {"right_closed", "both_closed"})
        current = p.amplitude if active else 0.
        if abs(t-p.onset)<1e-9:
            checkpoints["onset"] = y.tolist()
        if abs(t-right)<1e-9:
            checkpoints["offset"] = y.tolist()
        dose += current*dt
        old = y.copy()
        if method == "euler":
            y += dt*rhs(old, current, p)
        elif method == "semi_implicit":
            y[0] += dt*rhs(old, current, p)[0]
            y[1] += dt*p.a*(p.b*y[0]-y[1])
        elif method in {"historical", "historical_old_u"}:
            y[0] += dt/2*rhs(y, current, p)[0]
            y[0] += dt/2*rhs(y, current, p)[0]
            v_for_u = y[0] if method == "historical" else old[0]
            y[1] += dt*p.a*(p.b*v_for_u-y[1])
        else:
            raise ValueError(method)
        if y[0] >= p.peak:
            before = y.copy()
            y[0], y[1] = p.c, y[1]+p.d
            events.append(event_record(t+dt, before, y, p))
        trace.append((t+dt, *y))
        if not np.isfinite(y).all():
            raise RuntimeError(f"Nonfinite {method}, h={step}")
    return dict(method=method, hold="none", step_ms=step, endpoints=endpoints,
                parameters=asdict(p), events=events, checkpoints=checkpoints,
                final=y.tolist(), dose_current_ms=dose, trace=trace)


def stripped(result):
    return {key: value for key, value in result.items() if key != "trace"}


def row(name, result):
    onset = result["parameters"]["onset"]
    events = result["events"]
    selected = [e for e in events if e["time_ms"] >= onset]
    return dict(case=name, method=result["method"], step_ms=result["step_ms"],
                hold=result.get("hold", "none"), endpoints=result.get("endpoints", "exact"),
                n_before=sum(e["time_ms"] < onset for e in events), n_response=len(selected),
                first_latency_ms=selected[0]["latency_ms"] if selected else None,
                second_latency_ms=selected[1]["latency_ms"] if len(selected)>1 else None,
                dose_current_ms=result["dose_current_ms"],
                onset_v=result["checkpoints"].get("onset", [None, None])[0],
                onset_u=result["checkpoints"].get("onset", [None, None])[1],
                offset_v=result["checkpoints"].get("offset", [None, None])[0],
                offset_u=result["checkpoints"].get("offset", [None, None])[1],
                **{key: value for key, value in result["parameters"].items() if key not in {"after"}})


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)+"\n", encoding="utf-8")


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def cpp(cli, step_us, p=Case(), name=None, equilibrium_u=False):
    """Run the production CLI; derive expected pulse dose from its documented
    midpoint/fmod source expression and exported parameters (not an I trace).
    """
    name = name or f"cpp_{step_us:g}us"
    base = json.loads((STUDIO/"examples"/"two_inputs.json").read_text(encoding="utf-8"))
    neuron = dict(name="Audit isolated RS", px=.5, py=.5, voltage=p.v0, current=0.,
                  amplitude=p.amplitude, frequency=1., start=p.onset/1000,
                  stop=(p.onset+p.width)/1000, width=p.width/1000,
                  model="izhikevich", izh_a=p.a, izh_b=p.b, izh_c=p.c, izh_d=p.d,
                  izh_peak=p.peak, izh_u=p.u0, equilibrium_u=equilibrium_u,
                  individual_noise=False, noise_sigma=0.)
    base.update(schema=2, name=name, dt=step_us*1e-6, duration=p.end/1000,
                sample=max(step_us*1e-6, p.end/1000/80000), noise_sigma=0.,
                noise_interval=.001, seed=42, neurons=[neuron], synapses=[], astrocytes=[], gaps=[])
    config_path = RAW/f"{name}.json"
    prefix = RAW/name
    write_json(config_path, base)
    completed = subprocess.run([str(cli), "--config", str(config_path), "--output", str(prefix)],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
    (RAW/f"{name}.log").write_text(completed.stdout+completed.stderr, encoding="utf-8")
    if completed.returncode:
        raise RuntimeError(completed.stdout+completed.stderr)
    exported = json.loads(Path(str(prefix)+"_experiment.json").read_text(encoding="utf-8"))
    if canonical_config(exported) != canonical_config(base):
        raise AssertionError(f"JSON round-trip changed {name}")
    with Path(str(prefix)+"_events.csv").open(encoding="utf-8") as stream:
        times = [float(e["time_s"])*1000 for e in csv.DictReader(stream) if e["type"] == "spike"]
    trace = np.genfromtxt(str(prefix)+"_trace.csv", delimiter=",", names=True)
    actual_u0 = p.b*p.v0 if equilibrium_u else p.u0
    if trace["N1_V_mV"][0] != p.v0 or trace["N1_u"][0] != actual_u0:
        raise AssertionError("Initial-state JSON mapping is wrong")
    checkpoints = {}
    checkpoint_offsets = {}
    for key, time in [("onset", p.onset), ("offset", p.onset+p.width)]:
        index = int(np.argmin(np.abs(trace["time_s"]*1000-time)))
        checkpoints[key] = [float(trace["N1_V_mV"][index]), float(trace["N1_u"][index])]
        checkpoint_offsets[key] = float(trace["time_s"][index]*1000-time)
    dt = base["dt"]
    index = np.arange(math.ceil(base["duration"]/dt), dtype=float)
    old = index*dt
    new = np.minimum((index+1)*dt, base["duration"])
    mid = old+(new-old)/2
    active = ((mid >= neuron["start"]) & (mid < neuron["stop"]) &
              (np.fmod(mid-neuron["start"], 1/neuron["frequency"]) < neuron["width"]))
    dose = float(np.sum((new-old)[active])*p.amplitude*1000)
    return dict(method="C++ RK4/end-step reset", hold="none", step_ms=step_us/1000,
                parameters=asdict(replace(p, u0=actual_u0)), events=[dict(time_ms=t, latency_ms=t-p.onset) for t in times],
                checkpoints=checkpoints, checkpoint_sample_offsets_ms=checkpoint_offsets,
                dose_current_ms=dose, active_steps=int(np.sum(active)),
                json_roundtrip_exact=True, cli_summary=json.loads(Path(str(prefix)+"_summary.json").read_text()),
                trace=list(zip((trace["time_s"]*1000).tolist(), trace["N1_V_mV"].tolist(), trace["N1_u"].tolist())))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=STUDIO/"build"/"Release"/"network_studio_cli.exe")
    parser.add_argument("--quick", action="store_true", help="Skip parameter/initial-condition sweeps")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    RAW.mkdir(parents=True, exist_ok=True)
    results = {}

    def add(name, result):
        results[name] = result
        summary = row(name, result)
        print(f"{name}: N={summary['n_response']}, latency_ms="
              f"{summary['first_latency_ms']}, {summary['second_latency_ms']}; dose={summary['dose_current_ms']:.9g}", flush=True)
        return result

    for method in ["DOP853", "Radau"]:
        add(f"reference_{method}", exact(method=method))
    for method in ["euler", "historical", "semi_implicit", "historical_old_u"]:
        for step in [1., .5, .25, .1, .025]:
            add(f"{method}_{step:g}ms", fixed(step=step, method=method))
    for method in ["euler", "historical"]:
        for endpoints in ["both_closed", "both_open", "right_closed"]:
            add(f"{method}_1ms_{endpoints}", fixed(step=1., method=method, endpoints=endpoints))
    for hold in ["peak_frozen", "peak_recovery", "reset_frozen", "reset_recovery"]:
        add(f"hypothesis_hold_1ms_{hold}", exact(hold=hold))
    for step in [50., 25., 12.5, 5., 2., 1.]:
        add(f"cpp_{step:g}us", cpp(args.cli, step))
    # The saved manual u is intentionally inconsistent: equilibrium_u must win.
    add("cpp_equilibrium_overrides_manual", cpp(args.cli, 25., replace(Case(), u0=50.),
                                               "cpp_equilibrium_overrides_manual", equilibrium_u=True))
    if not args.quick:
        for onset in [0., 100., 1000.]:
            for v0 in [-70., -65., -60.]:
                for delta_u in [-5., 0., 5.]:
                    case = replace(Case(), onset=onset, v0=v0, u0=.2*v0+delta_u)
                    add(f"initial_onset{onset:g}_v{v0:g}_du{delta_u:g}", exact(case))
            if onset != 100.:
                add(f"reference_Radau_onset{onset:g}", exact(replace(Case(), onset=onset), method="Radau"))
                add(f"cpp_onset{onset:g}", cpp(args.cli, 25., replace(Case(), onset=onset), f"cpp_onset{onset:g}"))
        for parameter, values in dict(a=[.01, .04, .1], b=[.18, .22, .25, .27],
                                      c=[-70., -60., -55.], d=[2., 4., 10., 12.], peak=[0., 20., 40.]).items():
            for value in values:
                changes = {parameter: value}
                if parameter == "b":
                    changes["u0"] = value*Case().v0
                add(f"parameter_{parameter}_{value:g}", exact(replace(Case(), **changes)))
    reference = results["reference_DOP853"]
    radau = results["reference_Radau"]
    differences = [abs(a["time_ms"]-b["time_ms"]) for a, b in zip(reference["events"], radau["events"])]
    checks = dict(reference_counts_equal=len(reference["events"])==len(radau["events"]),
                  reference_max_event_difference_ms=max(differences, default=0),
                  cpp_counts_equal_reference=all(len(results[f"cpp_{step:g}us"]["events"])==len(reference["events"])
                                                 for step in [50.,25.,12.5,5.,2.,1.]),
                  cpp_dose_max_error=max(abs(results[f"cpp_{step:g}us"]["dose_current_ms"]-90.)
                                        for step in [50.,25.,12.5,5.,2.,1.]))
    if not checks["reference_counts_equal"] or checks["reference_max_event_difference_ms"] > 1e-6:
        raise AssertionError(f"Independent solvers disagree: {checks}")
    first = reference["events"][0]
    reset_state = [first["v_after"],first["u_after"]]
    offset_state = reference["checkpoints"]["offset"]
    mechanism = dict(first_reset=first, onset_state=reference["checkpoints"]["onset"],
                     offset_state=offset_state,
                     stimulus_remaining_after_first_reset_ms=103.-first["time_ms"],
                     dv_after_first_reset_with_current=rhs(reset_state,30.,Case())[0],
                     dv_after_first_reset_without_current=rhs(reset_state,0.,Case())[0],
                     dv_at_offset_after_current_removed=rhs(offset_state,0.,Case())[0])
    provenance = dict(created_utc=datetime.now(timezone.utc).isoformat(), python=platform.python_version(),
                      numpy=np.__version__, scipy=scipy.__version__,
                      cli_sha256=digest(args.cli), script_sha256=digest(Path(__file__)),
                      source_sha256={name:digest(STUDIO/name) for name in ["network.cpp","network.h","io.cpp","tests.cpp"]},
                      source_references=["https://www.izhikevich.org/publications/spikes.htm",
                                         "https://pmc.ncbi.nlm.nih.gov/articles/PMC8070389/"],
                      tolerance=dict(rtol=1e-11, atol=1e-12, max_step_ms=.5), checks=checks)
    write_json(OUT/"audit.json", dict(provenance=provenance, mechanism=mechanism,
                                      results={name:stripped(r) for name,r in results.items()}))
    write_csv(OUT/"comparison.csv", [row(name,result) for name,result in results.items()])
    write_csv(OUT/"reference_events.csv", reference["events"])
    write_csv(OUT/"reference_trace.csv", [dict(time_ms=t, v_mV=v, u=u) for t,v,u in reference["trace"]])
    # A focused plot is saved from independent solutions and actual CLI traces.
    import os
    os.environ.setdefault("MPLCONFIGDIR", str(RAW/"matplotlib_cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(3,1,figsize=(10,8),sharex=True,gridspec_kw={"height_ratios":[2.3,1.6,1.]})
    for name,label,style in [("reference_DOP853","DOP853: exact events","-"),
                              ("cpp_25us","C++ RK4: 25 us","--"),
                              ("euler_0.5ms","Euler: 0.5 ms",":")]:
        arr=np.asarray(results[name]["trace"])
        selection=(arr[:,0]>=99)&(arr[:,0]<=111)
        plotted=axes[0].plot(arr[selection,0]-100,arr[selection,1],style,label=label,lw=1.6)[0]
        axes[0].scatter([e["latency_ms"] for e in results[name]["events"]],
                        [30.]*len(results[name]["events"]),marker="v",s=28,color=plotted.get_color(),zorder=5)
        axes[1].plot(arr[selection,0]-100,arr[selection,2],style,lw=1.6)
    for axis in axes[:2]:
        axis.axvspan(0,3,color="#e4ad31",alpha=.14)
        axis.grid(alpha=.2)
    axes[0].set_ylabel("v, mV");axes[1].set_ylabel("u");axes[0].legend(loc="upper right")
    axes[2].step([-1,0,3,11],[0,30,0,0],where="post",color="#b37a08")
    axes[2].set_ylabel("I");axes[2].set_xlabel("Time relative to stimulus onset, ms")
    axes[2].set_ylim(-2,34);axes[2].set_xlim(-1,11);axes[2].grid(alpha=.2)
    fig.suptitle("Same RS parameters, same 30 x 3 ms input; triangles mark spike events")
    fig.tight_layout();fig.savefig(OUT/"numerical_comparison.png",dpi=160);plt.close(fig)
    event_text="; ".join(f"{e['latency_ms']:.9f} ms" for e in reference["events"])
    selected=[name for name in results if name.startswith(("reference_","euler_","historical_","cpp_","hypothesis_"))]
    lines=["# Independent audit: isolated Izhikevich pulse, I=30 for 3 ms", "",
           "An isolated neuron; a=0.02, b=0.2, c=-65, d=8, peak=30 mV; v(0)=-65, u(0)=-13.",
           "The stimulus is exactly [100,103) ms. Both exact solvers split at 100 and 103 ms and locate upward threshold events before applying the reset.", "",
           "This is one pulse after 100 ms without input, not the steady response to a periodic train. Its two-spike Euler 0.5 ms result cannot be generalized to later pulses, when the recovery variable still retains the preceding activity.",
           "The [original NeuroNet comparison](../neuronet_comparison/README.md) covers the separate 100 ms periodic protocol. With unchanged RS parameters, zero noise and Spike showing=1, the original program gives one steady spike per period at Step=0.5 ms and two at Step=0.025 ms. Its displayed frequencies 20/40 use a 50 ms counting window; they are not the long-run rates over a 100 ms stimulus period.", "",
           f"DOP853 response latencies: {event_text}. Radau maximum event-time difference: {checks['reference_max_event_difference_ms']:.3g} ms.",
           "", "| Case | h, ms | Response spikes | First latency, ms | Second latency, ms | Dose |", "|---|---:|---:|---:|---:|---:|"]
    for name in selected:
        r=row(name,results[name]);fmt=lambda x: "—" if x is None else f"{x:.8g}"
        lines.append(f"| {name} | {fmt(r['step_ms'])} | {r['n_response']} | {fmt(r['first_latency_ms'])} | {fmt(r['second_latency_ms'])} | {fmt(r['dose_current_ms'])} |")
    lines += ["", "The three-pool synapse and the other neuron were removed, so feedback cannot cause the second spike.",
              "The JSON file exported by every CLI run is compared structurally against its input; initial v/u and equilibrium-u precedence are checked explicitly.",
              "The C++ input dose is reconstructed from its exported configuration and midpoint/fmod source formula; it is not a separately measured current channel.",
              "C++ checkpoint values use the nearest recorded sample; their exact time offsets are included in audit.json.",
              "", "## Why the second spike occurs", "",
              f"Immediately before the first reset: v={first['v_before']:.12g} mV and u={first['u_before']:.12g}; immediately after: v={first['v_after']:.12g} mV and u={first['u_after']:.12g}.",
              f"The input remains on for {mechanism['stimulus_remaining_after_first_reset_ms']:.12g} ms after that reset. Its initial dv/dt is {mechanism['dv_after_first_reset_with_current']:.12g} mV/ms; without the input it would be {mechanism['dv_after_first_reset_without_current']:.12g} mV/ms.",
              f"At exactly 103 ms, v={offset_state[0]:.12g} mV and u={offset_state[1]:.12g}. Even after removing the current, dv/dt={mechanism['dv_at_offset_after_current_removed']:.12g} mV/ms. The second spike follows at {reference['events'][1]['time_ms']:.12g} ms.",
              "", "## Interpretation boundaries", "",
              "Euler, semi-implicit and the historical two-half-v-step scheme are numerically distinct. The historical scheme uses the updated v in the full u update. The old-u-order variant is included as an explicit hypothesis.",
              "both_closed includes both endpoint samples (four samples at h=1 ms); both_open omits both (two samples); right_closed gives three samples shifted by one step. Their doses therefore differ from, or are shifted relative to, [100,103).",
              "The hold rows are retained as rejected diagnostic hypotheses. They explicitly change the hybrid ODE by suppressing voltage evolution for 1 ms after a spike. Inspection of the original executable shows that v and u continue to be integrated while Spike showing/refractory_time gates event/output bookkeeping. These frozen-state hold models therefore do not describe that executable and do not explain its periodic result; see the [binary and experimental evidence](../neuronet_comparison/README.md).",
              "The 2021 NeuroNet article reports Euler integration with a 0.5 ms step. In this isolated-pulse audit Euler 0.5 ms produces two spikes. A periodic train has a different state at pulse onset: u has not fully recovered. Coarse Euler then registers and resets the first spike later, leaving less of the 3 ms input after reset; the state reached at pulse offset can relax instead of producing a second spike. The original program's one-spike steady train at 0.5 ms and two-spike train at 0.025 ms are consistent with this numerical and recovery-state dependence. Neither the isolated-pulse table nor the one-spike Euler 1 ms row establishes a need for an extra post-spike hold.",
              "", "The source audit finds correct millisecond-to-second conversion, RS coefficients, upward threshold, u+=d/v=c reset, strict JSON model selection, and a 3 ms midpoint-sampled stimulus at every audited C++ step. The C++ reset is located at step end, so spike times have a first-order event timing error despite the RK4 subthreshold solver.",
              "The existing pulse-count tests alone are not independent model validation. The adaptive-solver and parameter sensitivity results here validate the isolated-pulse hybrid ODE. They do not establish numerical equivalence to NeuroNet's coarse Euler periodic simulation; that comparison is documented separately.",
              "", "References: [Izhikevich original model and code](https://www.izhikevich.org/publications/spikes.htm); [Lobov et al., 2021](https://pmc.ncbi.nlm.nih.gov/articles/PMC8070389/).", "",
              "Generated by `audit_numerics.py`; see `audit.json` for hashes, exact reset states, parameter sweeps, and software versions."]
    (OUT/"README.md").write_text("\n".join(lines)+"\n",encoding="utf-8")
    print(json.dumps(checks, indent=2),flush=True)


if __name__ == "__main__":
    main()
