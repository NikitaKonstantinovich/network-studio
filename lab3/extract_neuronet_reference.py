"""Extract a small, auditable STP reference from the supplied NeuroNet snapshots.

Python standard library only. Reads original files, writes only the requested
JSON. No simulation is run. The reference is one presynaptic neuron's first
response, not a replacement for a complete NeuroNet frequency-response study.

    python extract_neuronet_reference.py --source PATH/NET_files/STP
"""

import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
DEFAULT_SOURCE = Path("C:/Users/N1kit/Downloads/1_NeuroNet/1_NeuroNet/NET_files/STP")
DEFAULT_PDF = Path("C:/Users/N1kit/Downloads/UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf")


def source_file(path):
    return {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def field(text, name):
    match = re.search(r"^" + re.escape(name) + r" = ([^\r\n]+)", text, flags=re.MULTILINE)
    if not match:
        raise ValueError(f"Missing field: {name}")
    return match.group(1).strip()


def number(text, name):
    result = float(field(text, name))
    if not math.isfinite(result):
        raise ValueError(f"Non-finite field: {name}")
    return result


def neuron_block(text, node):
    match = re.search(r"^Neuron ID = " + str(node) + r"\s*\n(.*?)(?=^Neuron ID = |^Number of stimulators = |\Z)",
                      text, flags=re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"Neuron {node} not found")
    return match.group(1)


def stimulus(text):
    section = text[text.index("Number of stimulators ="):]
    count = int(number(section, "Number of outputs"))
    intervals = int(number(section, "Interval table size"))
    return {"stimulator_count": int(number(section, "Number of stimulators")),
            "stimulator_id_1based": int(number(section, "Stimulator ID")),
            "target_neuron_ids_1based": [int(number(section, f"ID{i}")) for i in range(count)],
            "frame_duration_ms": number(section, "Frame duration"),
            "interval_table": [{"end_time_ms": number(section, f"i_time{i}"),
                                "voltage_setting": number(section, f"i_voltage{i}")}
                               for i in range(intervals)],
            "other_saved_fields": {key: number(section, key) for key in ("Pulse_amplitude", "Pulse_duration", "Touch_freq")},
            "interpretation_note": "Preserved interval-table and other pulse fields without assuming they are interchangeable."}


def extract(source, pdf, node):
    files = []
    for path in source.glob("*.net"):
        match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)s\.net", path.name)
        if match and 0 <= float(match.group(1)) <= .020:
            files.append((float(match.group(1)), path))
    files.sort()
    if not files:
        raise ValueError("No original 0..20-ms snapshots found")
    initial_text = files[0][1].read_text(encoding="cp1251")
    initial_node = neuron_block(initial_text, node)
    capacity = sum(number(initial_node, key) for key in ("parameter_x", "Output", "parameter_z"))
    if capacity <= 0:
        raise ValueError("Initial resource capacity must be positive")
    parameter_keys = ("parameter_U", "tau_recovery", "tau_I", "tau_facility", "refractory_time",
                      "parameter_a", "parameter_b", "parameter_c", "parameter_d")
    parameters = {key: number(initial_node, key) for key in parameter_keys}
    expected_stp = {"parameter_U": .5, "tau_recovery": 50., "tau_I": 10., "tau_facility": 1000.}
    if capacity != 20. or any(parameters[key] != value for key, value in expected_stp.items()):
        raise ValueError("This reference protocol expects capacity20, U=.5 and tau_rec/tau_I/tau_fac=50/10/1000ms")
    initial_stimulus = stimulus(initial_text)
    records = []
    for filename_time, path in files:
        text = path.read_text(encoding="cp1251")
        block = neuron_block(text, node)
        # NeuroNet writes the header clock in ms although filenames use s.
        model_time_ms = number(text, "Model time")
        time_s = model_time_ms / 1000.
        if not math.isclose(time_s, filename_time, rel_tol=0, abs_tol=1e-12):
            raise ValueError(f"Filename/model-time mismatch: {path.name}")
        if {key: number(block, key) for key in parameter_keys} != parameters:
            raise ValueError(f"Parameters changed in {path.name}")
        if stimulus(text) != initial_stimulus:
            raise ValueError(f"Stimulator settings changed in {path.name}")
        raw = {key: number(block, key) for key in
               ("parameter_x", "Output", "Output_old", "parameter_z", "parameter_u", "E_m", "E_m_old", "variable_U_e", "Time_cur_spike", "Is_spike")}
        total = raw["parameter_x"] + raw["Output"] + raw["parameter_z"]
        records.append({"time_s": time_s, "raw_model_time_ms": model_time_ms, "source": source_file(path), "raw": raw,
                        "normalized": {"x": raw["parameter_x"] / capacity, "y": raw["Output"] / capacity,
                                       "z": raw["parameter_z"] / capacity, "u": raw["parameter_u"]},
                        "raw_resource_sum": total, "raw_resource_sum_error": total - capacity})
    nonzero = [row for row in records if row["raw"]["Output"] > 0]
    if not nonzero:
        raise ValueError("No response found in the available snapshots")
    onset = nonzero[0]
    pre = [row for row in records if row["time_s"] < onset["time_s"]][-1]
    euler_residuals = []
    for previous, current in zip(nonzero, nonzero[1:]):
        delta_ms = (current["time_s"] - previous["time_s"]) * 1000
        old = previous["raw"]
        expected = {
            "Output": old["Output"] * (1 - delta_ms / parameters["tau_I"]),
            "parameter_x": old["parameter_x"] + delta_ms * old["parameter_z"] / parameters["tau_recovery"],
            "parameter_z": old["parameter_z"] + delta_ms * (old["Output"] / parameters["tau_I"] - old["parameter_z"] / parameters["tau_recovery"]),
            "parameter_u": old["parameter_u"] * (1 - delta_ms / parameters["tau_facility"]),
        }
        euler_residuals.append({"from_time_s": previous["time_s"], "to_time_s": current["time_s"],
                                "step_ms": delta_ms, "raw_field_residuals": {
                                    key: current["raw"][key] - value for key, value in expected.items()}})
    log_path = source / "calculate_0s.log"
    expected_times = [index * .0005 for index in range(41)]
    missing = [time_s for time_s in expected_times if not any(abs(row["time_s"] - time_s) < 1e-12 for row in records)]
    return {
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "extractor": source_file(Path(__file__)),
        "scope": f"Original saved state of presynaptic neuron {node}, one first response and its available early decay; no numerical simulation performed by this extractor.",
        "pdf_source": dict(source_file(pdf), relevant_pages=[5, 6, 7, 22, 23, 24]),
        "snapshot_directory": str(source.resolve()), "neuron_id_1based": node,
        "neuron_type": field(initial_node, "Type"), "neuron_class": field(initial_node, "Class"),
        "network_neuron_count": int(number(initial_text, "Number of neurons")),
        "saved_per_neuron_stdp_flag": "STDP_YES" if "STDP_YES" in initial_node else "STDP_NO",
        "run_log": dict(source_file(log_path), text=log_path.read_text(encoding="cp1251")),
        "parameters_raw": parameters,
        "parameter_units": {"tau_recovery": "ms", "tau_I": "ms", "tau_facility": "ms", "refractory_time": "ms",
                            "parameter_U": "dimensionless", "parameter_a": "Izhikevich millisecond convention",
                            "parameter_b": "Izhikevich convention", "parameter_c": "mV", "parameter_d": "recovery-variable units"},
        "state_units": {"time_s": "s", "raw_model_time_ms": "ms (original Model time header)", "parameter_x": "scaled resource/output units", "Output": "scaled resource/output units",
                        "Output_old": "scaled resource/output units", "parameter_z": "scaled resource/output units", "parameter_u": "dimensionless",
                        "E_m": "mV", "E_m_old": "mV", "variable_U_e": "Izhikevich recovery-variable units",
                        "Time_cur_spike": "ms", "Is_spike": "saved state flag, not an independent event counter"},
        "stimulator": initial_stimulus,
        "normalization": {"raw_resource_capacity": capacity, "normalized_resource_capacity": 1,
                          "gain_for_normalized_model": 20,
                          "mapping": "x=parameter_x/20, y=Output/20, z=parameter_z/20; u=parameter_u. The normalized current/output uses g=20 ONCE.",
                          "evidence": "Initial x+Output+z=20; the same total is preserved across the saved sequence within decimal serialization precision. PDF p.22 specifies default g=20.",
                          "maximum_resource_sum_error": max(abs(row["raw_resource_sum_error"]) for row in records)},
        "pre_spike_reference": {"time_s": pre["time_s"], "raw_x": pre["raw"]["parameter_x"],
                                "raw_y": pre["raw"]["Output"], "raw_z": pre["raw"]["parameter_z"], "u": pre["raw"]["parameter_u"]},
        "first_observed_response": {"time_s": onset["time_s"], "raw_output": onset["raw"]["Output"],
                                    "normalized_y": onset["normalized"]["y"], "updated_u": onset["raw"]["parameter_u"],
                                    "interpretation": "u changes from 0 to U=.5 before release; raw x=20 gives a raw output jump 10, equivalent to normalized release .5 and gain20."},
        "available_interval_s": [records[0]["time_s"], records[-1]["time_s"]],
        "requested_interval_s": [0., .020], "missing_half_ms_times_s": missing,
        "records": records, "explicit_euler_decay_residuals": euler_residuals,
        "limitations": [
            "The files show a fully recovered initialized state, not an observed >5-second silent history.",
            f"The first response belongs to neuron{node} in a connected 500-neuron network; directly stimulated neuron IDs are listed in the stimulus metadata. It is not an isolated two-neuron experiment.",
            "No trace is supplied at 10.5..20 ms in this snapshot series. Later files are isolated snapshots of longer runs and are excluded.",
            "Snapshot Is_spike can remain1 across successive files; consecutive nonzero flags must not be counted as separate spikes.",
            "The local calculate_0s.log records noise0 and learning-rate0. Per-neuron STDP_YES alone does not establish that learning was active.",
            "Observed decay agrees with explicit Euler at0.5ms within serialized-state precision. This does not establish every branch, delay, or stimulus detail of NeuroNet's numerical implementation.",
            "Output is the presynaptic scaled synaptic signal. A postsynaptic voltage maximum, synaptic current multiplied by an arbitrary target weight, and an event release increment are different observables.",
            "Only positive tau_facility=1000ms and U=.5 occur in these snapshots; the tau_facility=0 implementation cannot be inferred from them.",
            "This reference supports the first amplitude, normalization, event ordering and early synaptic decay; it does not determine the full steady-state frequency response or recruitment-area curve."
        ]
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--pdf", type=Path, default=DEFAULT_PDF)
    parser.add_argument("--output", type=Path, default=HERE / "neuronet_snapshot_reference.json")
    parser.add_argument("--neuron", type=int, default=189)
    args = parser.parse_args()
    reference = extract(args.source, args.pdf, args.neuron)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(reference, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"Saved {len(reference['records'])} snapshots for neuron {args.neuron}: {args.output}")
    print(json.dumps({"first_response": reference["first_observed_response"], "available_interval_s": reference["available_interval_s"],
                      "maximum_resource_sum_error": reference["normalization"]["maximum_resource_sum_error"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
