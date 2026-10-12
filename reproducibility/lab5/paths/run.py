"""Native matched-seed path experiment for manual lab 5, task 3.

Only writes this directory. No production rebuild or model changes.
"""
from __future__ import annotations
import argparse
import collections
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[2]
TOPOLOGIES = {"two": [(0, 1)], "chain": [(0, 1), (1, 2)],
              "shortcut": [(0, 1), (1, 2), (0, 2)]}
PARAMETERS = dict(C_m=1., g_Na=120., g_K=36., g_L=.3, E_Na=50., E_K=-77.,
    E_L=-54.387, V_spike=0., Y_release=1000., Omega_Y=40., O_N=.3,
    Omega_N=.5, O_beta=.5, O_delta=.02, kappa_delta=1.5, K_delta=.1,
    O_3K=2., K_D=.7, K_3K=1., Omega_5P=.04, Omega_C=6., Omega_L=.11,
    O_P=.9, K_P=.1, C_T=2., rho_A=.185, O_2=.2, d1=.13, d2=1.049,
    d3=.9434, d5=.08234, U_A=.6, tau_A=1.5, G_release=5., Omega_G=1.,
    O_G=1.5, Omega_S=.05, K_SIC=1., E_syn=0.)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def config(topology, seed, noise, dt):
    name = f"{topology}_seed{seed:02d}_noise{noise:g}_dt{dt * 1e6:g}us"
    count = 2 if topology == "two" else 3
    nodes = []
    for i in range(count):
        nodes.append(dict(name=f"N{i + 1} (NeuroNet {i + 5})", px=.2 + .3*i,
            py=.5 if i != 1 else .3, voltage=-65., current=0., amplitude=25. if i == 0 else 0.,
            frequency=10., start=.05, stop=5., width=.003, model="izhikevich",
            izh_a=.02, izh_b=.2, izh_c=-65., izh_d=8., izh_peak=30., izh_u=-13.,
            equilibrium_u=True, individual_noise=False, noise_sigma=0., inhibitory=False))
    edges = []
    for pre, post in TOPOLOGIES[topology]:
        edges.append(dict(source=pre, target=post, astro=-1, conductance=.25, reversal=0.,
            release=.5, tau_syn=.010, tau_f=1., tau_d=.050, spill=.01, enabled=True,
            model="tsodyks_markram", weight=.1, gain=20., stdp=True,
            a_plus=.001, a_minus=.001, tau_plus=.010, tau_minus=.010,
            weight_min=0., weight_max=1., stdp_rule="multiplicative"))
    return dict(schema=4, name=f"Lab 5 task 3 / {name}", dt=dt, duration=5., sample=.0001,
        noise_sigma=noise, noise_interval=.001, seed=seed, record_all=True,
        record_neurons=[], record_synapses=[], parameters=copy.deepcopy(PARAMETERS),
        neurons=nodes, astrocytes=[], synapses=edges, gaps=[])


def stdp_replay(c, events):
    """Explicit all-prior-pairs oracle; no recurrent native trace algorithm."""
    by_time = collections.defaultdict(set)
    for t, node in events:
        by_time[t].add(node)
    previous = [[] for _ in c["neurons"]]
    weights = [s["weight"] for s in c["synapses"]]
    for t, nodes in sorted(by_time.items()):
        for j, s in enumerate(c["synapses"]):
            pre, post = s["source"], s["target"]
            up = math.fsum(math.exp(-(t-p)/s["tau_plus"]) for p in previous[pre]) if post in nodes else 0.
            down = math.fsum(math.exp(-(t-p)/s["tau_minus"]) for p in previous[post]) if pre in nodes else 0.
            w = weights[j]
            delta = s["a_plus"]*(s["weight_max"]-w)*up - s["a_minus"]*(w-s["weight_min"])*down
            weights[j] = min(s["weight_max"], max(s["weight_min"], w+delta))
        for node in nodes:
            previous[node].append(t)
    return weights


def metrics(c, prefix):
    summary = read(prefix.with_name(prefix.name + "_summary.json"))
    snapshot = read(prefix.with_name(prefix.name + "_experiment.json"))
    assert snapshot == c, "Exported complete schema-4 configuration differs"
    assert summary["complete"] and not summary["error"]
    assert abs(summary["time_s"]-5.) < 1e-12
    assert summary["steps"] == round(5./c["dt"])
    events = []
    node_events = [[] for _ in c["neurons"]]
    with prefix.with_name(prefix.name + "_events.csv").open(newline="", encoding="utf-8") as f:
        previous = -1.
        for row in csv.DictReader(f):
            t = float(row["time_s"])
            assert math.isfinite(t) and previous <= t <= 5.
            assert row["type"] == "spike"
            node = int(row["node_index_1based"])-1
            events.append((t, node)); node_events[node].append(t); previous=t
    counts = list(map(len, node_events))
    assert counts == summary["neuron_spikes"]
    max_v = [-math.inf] * len(c["neurons"])
    max_after_input_v = max_v.copy()
    trace_rows = 0
    with prefix.with_name(prefix.name + "_trace.csv").open(newline="", encoding="utf-8") as f:
        previous = -1.
        for row in csv.DictReader(f):
            values = {k: float(v) for k, v in row.items()}
            assert all(map(math.isfinite, values.values()))
            t = values["time_s"]
            assert t > previous and t <= 5.
            previous=t; trace_rows+=1
            for i in range(len(max_v)):
                v = values[f"N{i+1}_V_mV"]
                max_v[i] = max(max_v[i],v)
                if t >= .05:
                    max_after_input_v[i] = max(max_after_input_v[i],v)
        assert abs(previous-5.) < 1e-12 and trace_rows == 50001
    initial_weights = [s["weight"] for s in c["synapses"]]
    minimum_weights, maximum_weights = initial_weights.copy(), initial_weights.copy()
    with prefix.with_name(prefix.name + "_weights.csv").open(newline="", encoding="utf-8") as f:
        n=0; previous=-1.
        for row in csv.DictReader(f):
            t=float(row["time_s"])
            assert math.isfinite(t) and t>previous and t<=5.
            previous=t; n+=1
            for j in range(len(initial_weights)):
                w=float(row[f"S{j+1}_weight"])
                assert math.isfinite(w) and 0 <= w <= 1
                minimum_weights[j]=min(minimum_weights[j],w)
                maximum_weights[j]=max(maximum_weights[j],w)
        assert n==trace_rows and abs(previous-5.)<1e-12
        final_weights=[float(row[f"S{j+1}_weight"]) for j in range(len(initial_weights))]
    assert final_weights==summary["final_synapse_weights"]
    independent=stdp_replay(c,events)
    stdp_error=max(abs(a-b) for a,b in zip(final_weights,independent))
    assert stdp_error < 1e-10
    return dict(spikes=counts, spike_times_s=node_events, max_sampled_V_mV=max_v,
        max_sampled_V_after_input_mV=max_after_input_v, final_weights=final_weights,
        deltas=[w-.1 for w in final_weights], minimum_weights=minimum_weights,
        maximum_weights=maximum_weights, independent_stdp_max_error=stdp_error,
        intermediate_spikes=counts[1] if len(counts)==3 else None,
        indirect_path_has_both_neurons_spiking=bool(counts[1] and counts[2]) if len(counts)==3 else None,
        trace_rows=trace_rows, complete=True,
        hashes={p.name:sha(p) for p in sorted(prefix.parent.glob(prefix.name+"_*"))})


def describe(values):
    return dict(min=min(values), max=max(values), mean=statistics.mean(values),
                sample_sd=statistics.stdev(values) if len(values)>1 else 0.)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    frozen_cli=HERE.parent/"pairs/baseline/network_studio_cli.exe"
    parser.add_argument("--cli", type=Path, default=frozen_cli if frozen_cli.exists() else STUDIO/"build/Release/network_studio_cli.exe")
    parser.add_argument("--skip-run", action="store_true", help="Recheck existing raw files with matching provenance")
    args=parser.parse_args(); cli=args.cli.resolve()
    old=read(HERE/"findings.json") if args.skip_run else None
    cli_sha=sha(cli)
    baseline=read(HERE.parent/"baseline_sources/manifest.json")
    assert cli_sha==baseline["cli_sha256"], "This strict baseline replay requires the archived pre-triplet CLI"
    baseline_sources={entry["file"]:entry["sha256"] for entry in baseline["files"]}
    if old:
        assert old["provenance"]["cli_sha256"]==cli_sha, "CLI changed since the saved run"
    cases=[]
    for topology in TOPOLOGIES:
        for seed in range(10):
            cases.append((topology,seed,3.,.000025,"main"))
        cases.append((topology,0,0.,.000025,"no_noise"))
        cases.append((topology,0,3.,.0000125,"half_step"))
    output={}
    for topology,seed,noise,dt,kind in cases:
        c=config(topology,seed,noise,dt)
        name=f"{topology}_seed{seed:02d}_noise{noise:g}_dt{dt*1e6:g}us"
        cp=HERE/"configs"/(name+".json"); prefix=HERE/"raw"/name
        if args.skip_run:
            assert read(cp)==c
            expected=old["runs"][name]
            assert sha(cp)==expected["config_sha256"]
            assert all(sha(prefix.parent/p)==h for p,h in expected["metrics"]["hashes"].items())
            elapsed=expected["wall_seconds"]
        else:
            save(cp,c); prefix.parent.mkdir(parents=True,exist_ok=True)
            before=time.perf_counter()
            r=subprocess.run([str(cli),"--config",str(cp),"--output",str(prefix)],text=True,
                             capture_output=True,encoding="utf-8",check=True)
            elapsed=time.perf_counter()-before
            prefix.with_name(name+"_stdout.txt").write_text(r.stdout,encoding="utf-8")
        m=metrics(c,prefix)
        output[name]=dict(topology=topology,seed=seed,noise_sigma=noise,dt_s=dt,kind=kind,
            config=str(cp.relative_to(HERE)),config_sha256=sha(cp),wall_seconds=elapsed,metrics=m)
        print(name,"spikes",m["spikes"],"weights",m["final_weights"],f"wall {elapsed:.2f}s",flush=True)
    aggregate={}
    for topology in TOPOLOGIES:
        rows=[r["metrics"] for r in output.values() if r["topology"]==topology and r["kind"]=="main"]
        aggregate[topology]=dict(replicates=len(rows),
            spikes_by_node=[describe([r["spikes"][i] for r in rows]) for i in range(len(rows[0]["spikes"]))],
            final_weights_by_edge=[describe([r["final_weights"][i] for r in rows]) for i in range(len(rows[0]["final_weights"]))],
            max_sampled_V_by_node=[describe([r["max_sampled_V_mV"][i] for r in rows]) for i in range(len(rows[0]["spikes"]))],
            any_postsynaptic_spiking_runs=sum(any(r["spikes"][1:]) for r in rows))
    convergence={}
    for topology in TOPOLOGIES:
        coarse=output[f"{topology}_seed00_noise3_dt25us"]["metrics"]
        fine=output[f"{topology}_seed00_noise3_dt12.5us"]["metrics"]
        convergence[topology]=dict(coarse_spikes=coarse["spikes"],fine_spikes=fine["spikes"],
            maximum_weight_difference=max(abs(a-b) for a,b in zip(coarse["final_weights"],fine["final_weights"])),
            maximum_sampled_V_difference_by_node=[abs(a-b) for a,b in zip(coarse["max_sampled_V_mV"],fine["max_sampled_V_mV"])],
            paired_spike_time_max_difference_s_by_node=[max((abs(a-b) for a,b in zip(x,y)),default=0.) if len(x)==len(y) else None for x,y in zip(coarse["spike_times_s"],fine["spike_times_s"])])
    matched=[]
    for seed in range(10):
        a=output[f"chain_seed{seed:02d}_noise3_dt25us"]["metrics"]
        b=output[f"shortcut_seed{seed:02d}_noise3_dt25us"]["metrics"]
        matched.append(dict(seed=seed,source_events_equal=a["spike_times_s"][0]==b["spike_times_s"][0],
            intermediate_events_equal=a["spike_times_s"][1]==b["spike_times_s"][1],
            target_spike_difference=b["spikes"][2]-a["spikes"][2],
            shortcut_final_direct_weight=b["final_weights"][2],
            shortcut_indirect_weights=b["final_weights"][:2]))
    result=dict(task="Manual lab 5 / completed-series lab 4 / task 3",run_count=len(output),
        main_runs=30,no_noise_controls=3,half_step_controls=3,aggregate=aggregate,
        convergence=convergence,matched_chain_shortcut=matched,runs=output,
        reference=dict(source_report="labs/lab4_report/Отчёт_ЛР4.md, task 3",
            two_weights=[.1],chain_weights=[.1,.1],shortcut_weights=[.1,.1,.106812],
            evidence="Final GUI values and short activity frames; no full spike log; approximate 5s runs",
            known_noise_setting=3,unknown=["noise process and seed","exact duration of each stage",
                "dynamic state at stage start","conduction-delay mapping","exact NeuroNet STDP update implementation"]),
        limitations=["Gaussian independent per-node noise held 1ms; equal seeds match indexed nodes across native topologies, not NeuroNet realizations",
            "Each native run starts V=-65,u=-13,TM u=0,x=1,y=0 and empty STDP traces; original stage transitions may preserve prior states",
            "Native RK4 25us vs reported NeuroNet Step .500; native network has no conduction delays",
            "V maxima are sampled every0.1ms; threshold events are full solver event logs, not inferred from sampled peaks",
            "Changing paths without intermediate spiking cannot demonstrate weakening of an active competing indirect pathway"],
        provenance=dict(cli=str(cli),cli_sha256=cli_sha,script_sha256=sha(Path(__file__)),
            source_sha256={name:baseline_sources[name] for name in ["network.cpp","network.h","io.cpp"]},
            source_snapshot="../baseline_sources/manifest.json"))
    save(HERE/"findings.json",result)
    print("PASS",len(output),"native runs; exact configuration/summary/event/finite-state/weight checks",flush=True)


if __name__=="__main__":
    main()
