"""Non-destructive rerun of Laboratory 1; all products stay next to this file.

Run with the bundled Python and PYTHONPATH pointing to tmp/python_deps.
Historical configs/results and production sources are read only.
"""
from __future__ import annotations
import argparse
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import time

OUT = Path(__file__).resolve().parent
STUDIO = OUT.parents[2]
LAB = STUDIO / "lab1"
CLI = STUDIO / "build/Release/network_studio_cli.exe"
sys.path.insert(0, str(LAB))
sys.path.insert(0, str(STUDIO / "lab3"))
import reproduce as lab
from config_compat import canonical_config


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False)+"\n", encoding="utf-8")


def configure():
    lab.RESULTS = OUT / "analysis"
    lab.RAW = OUT / "raw"
    lab.FIGURES = OUT / "figures"
    lab.RESULTS.mkdir(parents=True, exist_ok=True)
    lab.imports()


def baseline():
    manifest = read(LAB / "results/manifest.json")
    scenarios = {name: read(LAB / "configs" / (name+".json")) for name in manifest["scenarios"]}
    lab.execute(CLI, scenarios)
    metrics = lab.analyze(scenarios, manifest["main"], manifest["deterministic"])
    comparisons = []
    for name in scenarios:
        for suffix in ["trace.csv", "events.csv"]:
            old = LAB / "results/raw" / f"{name}_{suffix}"
            new = lab.RAW / f"{name}_{suffix}"
            comparisons.append(dict(scenario=name, file=suffix, historical_sha256=digest(old),
                                    current_sha256=digest(new), byte_identical=digest(old)==digest(new)))
    result = dict(cli_sha256=digest(CLI), scenario_count=len(scenarios),
                  csv_comparisons=comparisons,
                  identical_csv_count=sum(row["byte_identical"] for row in comparisons),
                  metrics_structurally_identical=metrics == read(LAB / "results/metrics.json"),
                  checks=metrics["checks"], convergence=metrics["convergence"],
                  noise_replicates=metrics["noise_replicates"])
    write(OUT / "baseline.json", result)
    lab.plots(metrics)
    print("Baseline:", result["identical_csv_count"], "/",len(comparisons),"CSV identical; metrics identical",result["metrics_structurally_identical"], flush=True)


def run_extra(name, config):
    dest = OUT / "extra"
    path = dest / f"{name}.json"
    write(path, config)
    run = subprocess.run([str(CLI), "--config", str(path), "--output", str(dest/name)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    (dest/f"{name}.log").write_text(run.stdout+run.stderr, encoding="utf-8")
    if run.returncode:
        raise RuntimeError(f"{name}: {run.stderr}")
    summary = read(dest/f"{name}_summary.json")
    assert summary["complete"] and not summary["error"]
    assert canonical_config(config)==canonical_config(read(dest/f"{name}_experiment.json"))
    return dest/name


def events(prefix, index=1):
    with Path(str(prefix)+"_events.csv").open(encoding="utf-8-sig", newline="") as stream:
        return [float(row["time_s"]) for row in csv.DictReader(stream)
                if row["type"]=="spike" and int(row["node_index_1based"])==index]


def adaptive(config, method="DOP853"):
    """Independent event-located hybrid ODE, in ms; no native solver import.

    Intended for uncoupled source neuron 1 only. All discontinuity boundaries
    are split exactly, unlike the production grid midpoint input/reset.
    """
    import numpy as np
    from scipy.integrate import solve_ivp
    n = config["neurons"][0]
    assert config["noise_sigma"]==0 and not n["individual_noise"]
    assert not any(s["enabled"] and s["target"]==0 for s in config["synapses"])
    end = config["duration"]*1000
    boundaries = {0.,end}
    intervals = []
    if n["frequency"] > 0 and n["amplitude"] != 0:
        for i in range(math.ceil(config["duration"]*n["frequency"])+1):
            left = n["start"]*1000+i*1000/n["frequency"]
            if left >= min(end,n["stop"]*1000)-1e-9:
                break
            right = min(left+n["width"]*1000,n["stop"]*1000,end)
            intervals.append((left,right))
            boundaries.update((left,right))
    y = [n["voltage"], n["izh_b"]*n["voltage"] if n["equilibrium_u"] else n["izh_u"]]
    spike_times, resets = [], []
    def peak(t,y):
        return y[0]-n["izh_peak"]
    peak.direction = 1
    peak.terminal = True
    edges = sorted(boundaries)
    for left,right in zip(edges,edges[1:]):
        mid = (left+right)/2
        current = n["current"] + (n["amplitude"] if any(a<=mid<b for a,b in intervals) else 0)
        def rhs(t,y):
            v,u = y
            return [.04*v*v+5*v+140-u+current, n["izh_a"]*(n["izh_b"]*v-u)]
        t = left
        while t < right-1e-10:
            sol = solve_ivp(rhs,(t,right),y,events=peak,method=method,rtol=1e-11,atol=1e-12,max_step=.5)
            assert sol.success, sol.message
            t = float(sol.t[-1]); y = sol.y[:,-1].copy()
            if len(sol.t_events[0]):
                before = y.tolist()
                spike_times.append(t/1000)
                y = np.array([n["izh_c"],y[1]+n["izh_d"]])
                resets.append(dict(time_s=t/1000,before=before,after=y.tolist()))
    return dict(method=method, events_s=spike_times, resets=resets, final_state=y.tolist())


def comparison(actual, expected):
    match = len(actual)==len(expected)
    return dict(actual_spikes=len(actual), reference_spikes=len(expected), same_spike_count=match,
                max_event_error_ms=max((1000*abs(a-b) for a,b in zip(actual,expected)),default=0.) if match else None)


def independent():
    import numpy as np
    import scipy
    from reference_tm import state_at, tm_responses
    manifest = read(LAB / "results/manifest.json")
    result = dict(cli_sha256=digest(CLI), python=sys.version, numpy=np.__version__, scipy=scipy.__version__,
                  reference_protocol="solve_ivp DOP853; milliseconds; rtol=1e-11, atol=1e-12, max_step=.5ms; exact input boundaries and v=30 reset events", cases={})
    extra = {}
    for hz in [2,20]:
        c=read(LAB/"configs"/f"04_periodic_{hz:02}hz.json")
        c["name"]=f"Lab1 recheck: original 30 x 3 ms / {hz} Hz"
        c["neurons"][0]["width"] = .003
        key=f"reference_{hz:02}hz_30x3ms"
        run_extra(key,c);extra[key]=c
    case_names=manifest["deterministic"]+["02_reference_30x3ms","04_reference_10hz_30x3ms"]+list(extra)
    for name in case_names:
        print("Independent ODE:",name,flush=True)
        c = extra[name] if name in extra else read(LAB/"configs"/(name+".json"))
        prefix = (OUT/"extra" if name in extra else lab.RAW)/name
        ref = adaptive(c)
        write(OUT/"reference"/(name+".json"),ref)
        cases = dict(reference_events_s=ref["events_s"], native25us=comparison(events(prefix),ref["events_s"]))
        if name in manifest["deterministic"]:
            cases["native12_5us"]=comparison(events(lab.RAW/("fine_"+name)),ref["events_s"])
        else:
            fine = copy.deepcopy(c);fine["dt"] = .0000125
            fine_prefix = run_extra(name+"_12p5us",fine)
            cases["native12_5us"] = comparison(events(fine_prefix),ref["events_s"])
        if name in ["03_autonomous","05_dc_4","reference_20hz_30x3ms"]:
            finer=copy.deepcopy(c);finer["dt"] = .000005
            finer_prefix=run_extra(name+"_5us",finer)
            cases["native5us"]=comparison(events(finer_prefix),ref["events_s"])
        if name=="02_reference_30x3ms":
            radau=adaptive(c,"Radau")
            write(OUT/"reference"/(name+"_Radau.json"),radau)
            cases["radau_vs_dop853"]=comparison(radau["events_s"],ref["events_s"])
            for us in [50,5,2,1]:
                finer=copy.deepcopy(c);finer["dt"]=us*1e-6
                cases[f"native{us}us"]=comparison(events(run_extra(name+f"_{us}us",finer)),ref["events_s"])
        assert all(v["same_spike_count"] for k,v in cases.items() if isinstance(v,dict)), name
        result["cases"][name]=cases
        write(OUT/"independent.json",result)
    result["TM_analytic"]={}
    for name in ["02_single_pulse","02_reference_30x3ms"]:
        c=read(LAB/"configs"/(name+".json"));s=c["synapses"][0]
        data=lab.trace(name);spikes=events(lab.RAW/name)
        pars=dict(U=s["release"],tau_fac=s["tau_f"],tau_rec=s["tau_d"],tau_syn=s["tau_syn"],gain=s["gain"])
        # Round only sample/event timestamps to a common native tick to avoid
        # binary64 decimal serialization placing an equal-time sample before event.
        times=[round(float(t)/c["dt"])*c["dt"] for t in data["time_s"]]
        ticks=[round(t/c["dt"])*c["dt"] for t in spikes]
        exact=state_at(ticks,times,**pars)
        errors={field:float(np.max(np.abs(data["S1_"+field]-np.array([r[field] for r in exact])))) for field in ["u","x","y"]}
        assert max(errors.values())<1e-8, errors
        result["TM_analytic"][name]=dict(samples=len(data),event_times_s=spikes,max_absolute_error=errors,events=tm_responses(ticks,**pars))
    c=read(LAB/"configs/07_noise_individual.json")
    c["name"]="Lab1 individual noise zero overrides global eight"
    c["noise_sigma"]=8.
    c["neurons"][0].update(individual_noise=True,noise_sigma=0.)
    c["neurons"][1].update(individual_noise=False,noise_sigma=0.)
    p=run_extra("individual_zero_global8",c)
    result["individual_zero_override"]=dict(zero_sigma_spikes=len(events(p,1)),inherited_sigma8_spikes=len(events(p,2)))
    assert result["individual_zero_override"]["zero_sigma_spikes"]==0
    assert result["individual_zero_override"]["inherited_sigma8_spikes"]>0
    result["noise_timestep_sensitivity"]={}
    for sigma in [5,8]:
        name=f"noise_sigma{sigma}_seed42"
        c=read(LAB/"configs"/(name+".json"));c["dt"]*=.5
        p=run_extra(name+"_12p5us",c)
        old=events(lab.RAW/name);new=events(p)
        result["noise_timestep_sensitivity"][str(sigma)]=comparison(new,old)
    write(OUT/"independent.json",result)
    print("Independent ODE/TM/noise checks completed",flush=True)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("phase",choices=["baseline","independent"])
    args=parser.parse_args()
    before=digest(CLI)
    configure()
    started=time.perf_counter()
    {"baseline":baseline,"independent":independent}[args.phase]()
    assert before==digest(CLI),"CLI changed during audit"
    print(args.phase,"elapsed",round(time.perf_counter()-started,3),flush=True)


if __name__=="__main__":
    main()
