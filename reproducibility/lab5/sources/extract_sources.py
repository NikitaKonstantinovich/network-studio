"""Read-only extraction of the local NeuroNet evidence for manual lab 5.

Writes only source_evidence.json beside this script. No simulation is performed.
Usage: python extract_sources.py [--neuronet PATH] [--workspace PATH]
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
NUMBER = r"[-+\d.eE]+"

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def source(path):
    return {"path": path.as_posix(), "sha256": sha(path), "bytes": path.stat().st_size}

def val(text, key):
    match = re.search(r"^" + re.escape(key) + r"\s*=\s*([^\r\n]+)", text, re.M)
    if not match:
        return None
    raw = match.group(1).strip()
    try:
        return float(raw)
    except ValueError:
        return raw

def parse_net(path):
    text = path.read_text(encoding="cp1251")
    result = {**source(path), "model_time_ms": val(text, "Model time"),
              "neurons": [], "stimulators": []}
    for block in re.findall(r"(?ms)^Neuron ID = .*?(?=^Neuron ID = |^Number of stimulators = |\Z)", text):
        keys = ["Neuron ID", "Type", "Class", "CoordinateX", "CoordinateY", "refractory_time",
                "E_m", "E_m_old", "variable_U_e", "Output", "Output_old", "Time_cur_spike", "Is_spike",
                "parameter_a", "parameter_b", "parameter_c", "parameter_d", "parameter_x", "parameter_z",
                "parameter_u", "parameter_U", "tau_recovery", "tau_I", "tau_facility", "Output_deque size"]
        n = {key: val(block, key) for key in keys}
        n["STDP_YES"] = "STDP_YES" in block
        n["inputs"] = [{"from": int(a), "weight": float(b)} for a, b in
                       re.findall(r"^ID\d+ = (\d+)\s+weight\d+ = (" + NUMBER + ")", block, re.M)]
        n["Output_deque"] = [float(a) for a in re.findall(r"^o_d\d+ = (" + NUMBER + ")", block, re.M)]
        n["resource_sum_x_Output_z"] = sum(n[k] for k in ("parameter_x", "Output", "parameter_z"))
        result["neurons"].append(n)
    for block in re.findall(r"(?ms)^Stimulator ID = .*?(?=^Stimulator ID = |^Number of group stimulators = |\Z)", text):
        s = {key: val(block, key) for key in ["Stimulator ID", "CoordinateX", "CoordinateY", "Frame duration",
                "Pulse_amplitude", "Pulse_duration", "Output", "Output_old"]}
        s["targets"] = [int(a) for a in re.findall(r"^ID\d+ = (\d+)\s*$", block, re.M)]
        s["table_end_ms_voltage"] = [[float(a), float(b)] for a, b in re.findall(
            r"i_time\d+ = (" + NUMBER + r")\s+i_voltage\d+ = (" + NUMBER + ")", block)]
        left = 0.
        s["active_intervals_ms_voltage"] = []
        for end, voltage in s["table_end_ms_voltage"]:
            if voltage:
                s["active_intervals_ms_voltage"].append([left, end, voltage])
            left = end
        result["stimulators"].append(s)
    return result

def parse_log(path):
    t = path.read_text(encoding="cp1251")
    return {**source(path), "filename_time_s": float(path.stem.split("_")[1][:-1]),
            "fields": dict(re.findall(r"^([^:\r\n]+):\s*([^\r\n]+)", t, re.M)),
            "rule_text": next((line for line in t.splitlines() if "based STDP" in line), None),
            "minus_plus_off": "Minus-plus is off" in t}

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--neuronet", type=Path, default=Path("C:/Users/N1kit/Downloads/1_NeuroNet/1_NeuroNet"))
    p.add_argument("--workspace", type=Path, default=Path("E:/asp/26_27"))
    args = p.parse_args()
    folder = args.neuronet / "3"
    nets = [parse_net(p) for p in sorted(folder.glob("*.net")) if p.stat().st_size < 50000]
    named = [n for n in nets if Path(n["path"]).name.startswith("lab4_")]
    logs = [parse_log(p) for p in sorted(folder.glob("calculate_*.log"))]
    for n in named:
        n["byte_identical_snapshot_paths"] = [v["path"] for v in nets if v["sha256"] == n["sha256"] and v["path"] != n["path"]]
        n["same_model_time_snapshot_paths"] = [v["path"] for v in nets if v["model_time_ms"] == n["model_time_ms"] and v["path"] != n["path"]]
        n["same_time_calculation_log_paths"] = [v["path"] for v in logs if abs(v["filename_time_s"]*1000 - n["model_time_ms"]) < 1e-6]
    report = args.workspace / "labs/lab4_report/Отчёт_ЛР4.md"
    # Independent .net values are compared with the report transcriptions below.
    # The images listed in visually_reviewed_screenshots were also inspected directly.
    cases = [
        (1, "pre_post_1Hz", 1, "pre-post", "pair", .501333, "03_t1_pre_post_1Hz.png"),
        (1, "post_pre_1Hz", 1, "post-pre", "pair", .0157814, "04_t1_post_pre_1Hz.png"),
        (1, "pre_post_10Hz", 10, "pre-post", "pair", .999748, "01_t1_pre_post_10Hz.png"),
        (1, "post_pre_10Hz", 10, "post-pre", "pair", .0000803577, "02_t1_post_pre_10Hz.png"),
        (1, "pre_post_40Hz", 40, "pre-post", "pair", .335617, "05_t1_pre_post_40Hz.png"),
        (1, "post_pre_40Hz", 40, "post-pre", "pair", .153155, "06_t1_post_pre_40Hz.png"),
        (2, "task2_pair_1Hz", 1, "pre-post", "pair", .147829, "10_t2_pair_1Hz.png"),
        (2, "task2_triplet_1Hz", 1, "pre-post", "triplet", .119444, "11_t2_triplet_1Hz.png"),
        (2, "task2_pair_10Hz", 10, "pre-post", "pair", .5437, "08_t2_pair_10Hz.png"),
        (2, "task2_triplet_10Hz", 10, "pre-post", "triplet", .459589, "09_t2_triplet_10Hz.png"),
        (2, "pre_post_pre_pair", 1, "pre-post-pre", "pair", .1141, "12_t2_pre_post_pre_pair.png"),
        (2, "pre_post_pre_triplet", 1, "pre-post-pre", "triplet_provisional", .0887917, "13_t2_pre_post_pre_triplet.png"),
        (2, "post_pre_post_pair", 1, "post-pre-post", "pair", .141625, "14_t2_post_pre_post_pair.png"),
        (2, "post_pre_post_triplet", 1, "post-pre-post", "triplet", .1523, "15_t2_post_pre_post_triplet.png"),
    ]
    measurements = []
    for task, stem, freq, protocol, rule, report_weight, screenshot in cases:
        snap = next(n for n in named if Path(n["path"]).name == f"lab4_{stem}.net")
        neuron = next(n for n in snap["neurons"] if n["Neuron ID"] == 6)
        weight = next(e["weight"] for e in neuron["inputs"] if e["from"] == 5)
        measurements.append({"task": task, "condition": stem, "stimulus_frequency_hz": freq,
             "protocol": protocol, "rule_reported": rule, "weight_initial_reported": .1,
             "weight_final_report": report_weight, "weight_final_net": weight,
             "net_report_equal": weight == report_weight, "relative_change_percent": (weight/.1-1)*100,
             "result_model_time_s": snap["model_time_ms"]/1000,
             "net_path": snap["path"], "net_sha256": snap["sha256"], "screenshot": screenshot,
             "training_elapsed_s": None, "training_elapsed_note": "Approximately 100 s reported; result timestamp is not duration.",
             "alpha_reported": 5 if task == 1 else 1, "learning_rate_reported": .001,
             "noise_sigma_reported": 0, "pre_resource_total_reported": 20 if task == 1 else 2,
             "post_resource_total_reported": 20})
    assert all(m["net_report_equal"] for m in measurements)
    reviewed = ["01_t1_pre_post_10Hz.png", "07_t1_double_spikes.png", "08_t2_pair_10Hz.png",
                "09_t2_triplet_10Hz.png", "11_t2_triplet_1Hz.png", "12_t2_pre_post_pre_pair.png",
                "13_t2_pre_post_pre_triplet.png", "15_t2_post_pre_post_triplet.png",
                "16_t2_pre_post_pre_validation.png", "18_t3_two_activity.png",
                "20_t3_chain_activity.png", "22_t3_shortcut_weights.png"]
    evidence = {
        "schema_version": 1,
        "scope": "Source extraction only; no simulator run or exact NeuroNet algorithm claim.",
        "manual": source(Path("C:/Users/N1kit/Downloads/UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf")),
        "manual_pages": [24,25,26,27], "theory_pages": [8,9],
        "report": source(report),
        "screenshot_sources": [source(p) for p in sorted((report.parent / "images").glob("[0-9][0-9]_*.png"))],
        "visually_reviewed_screenshots": reviewed,
        "measurements_task1_task2": measurements,
        "task3_time_evidence": {
            "two_neurons_initial_s": 11.675, "two_neurons_result_s": 17.775,
            "two_snapshot_difference_s": 6.1,
            "chain_initial_s": 17.775, "chain_snapshot_s": 18.515,
            "shortcut_all_weights_point_one_snapshot_s": 19.895, "shortcut_result_s": 32.420,
            "shortcut_snapshot_difference_s": 12.525,
            "note": "Timestamp differences are not proved uninterrupted learning durations. Chain endpoints and all intervening actions are not logged.",
            "two_final_weights": [[5,6,.1]], "chain_observed_weights": [[5,6,.1],[6,7,.1]],
            "shortcut_final_weights": [[5,6,.1],[6,7,.1],[5,7,.106812]]},
        "task3_intermediate_snapshots": [n for n in nets if Path(n["path"]).name in ["16.7s.net", "18.515s.net", "19.895s.net"]],
        "theory": {
            "pair": "Traces ds/dt=-s/tau+sum(delta); LTP=lambda*(1-w)*s_pre; LTD=-lambda*alpha*w*s_post.",
            "pair_defaults_page8": {"lambda": .001, "alpha": 5, "tau_ms": 10},
            "minimal_triplet_page9": "LTP=lambda*(1-w)*fast_pre*slow_post; LTD same as pair.",
            "triplet_recommendation_page9": {"lambda": .01, "alpha": 1, "tau_depression_ms": 10, "tau_fast_ms": 10, "tau_slow_ms": 100},
            "lab5_task2_override_pages25_26": {"lambda": .001, "alpha": 1},
            "ambiguities": ["Slow postsynaptic trace is not annotated t-minus vs t-plus in equation12.",
                            "Actual NeuroNet R05 implementation and arrival-vs-emission timing are unavailable.",
                            "Synaptic forgetting eq13 is described separately; lab5 does not request enabling it."]},
        "named_snapshots": named,
        "triplet_templates": [parse_net(p) for p in sorted((args.neuronet / "NET_files/STDP/triplet_STDP").glob("*.net"))],
        "calculation_logs": logs,
        "other_small_snapshots": [{k: v for k,v in n.items() if k not in ["neurons", "stimulators"]} |
                                  {"neuron_ids": [v["Neuron ID"] for v in n["neurons"]],
                                   "weights": [[v["from"], neuron["Neuron ID"], v["weight"]] for neuron in n["neurons"] for v in neuron["inputs"]]}
                                  for n in nets if not Path(n["path"]).name.startswith("lab4_")],
        "interpretation_notes": [
            "Model time in .net is milliseconds; filename/display time is seconds. A result timestamp is not an elapsed training duration.",
            "Each interval table voltage applies up to that row's end time; Pulse_amplitude/Pulse_duration are different controls and do not specify rhythmic pulses.",
            "STDP_YES is per-neuron permission; pair/triplet, learning rate, asymmetry and global noise are absent from .net and occur in calculation logs / screenshots.",
            "Log filename is an observed time label, not independently proven learning start or end. Exact-name time association does not prove uninterrupted mode.",
            "Initial x in GUI is resource total/scaling; serialized parameter_x is current available resource. x+Output+z approximately conserves that total.",
            "refractory_time=1 is serialized but GUI Izh label is Spike showing=1; real hard refractory behavior is not established by this field alone.",
            "Output_deque serializes delayed signal history. Mapping of velocity/geometry to transmission delay and pairing event time is not established.",
            "No local source files cpp/c/h/hpp/pas/dfm/dpr/cs were found under Downloads/1_NeuroNet. Numerical method and trace ordering cannot be derived from file names.",
            "Manual p27 direct path text 1->2 conflicts with common endpoints of 1->2->3; figures and report imply 1->3. Report realizes 5->7.",
        ]}
    (HERE / "source_evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(f"Extracted {len(named)} named snapshots, {len(logs)} calculation logs, {len(evidence['screenshot_sources'])} screenshots.")
    for n in named:
        weights = [(v["from"], neuron["Neuron ID"], v["weight"]) for neuron in n["neurons"] for v in neuron["inputs"]]
        print(Path(n["path"]).name, n["model_time_ms"], weights, [Path(v).name for v in n["same_time_calculation_log_paths"]])

if __name__ == "__main__":
    main()
