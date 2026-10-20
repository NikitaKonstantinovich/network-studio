"""Prepare explicit short or 150 -> 400 s scenarios; never run a simulation."""
import argparse
import copy
from pathlib import Path
import package as pkg


def create(cli, destination, laboratory=False, seeds=(42, 43), source=None):
    root = Path(__file__).resolve().parents[1]
    destination = Path(destination).resolve()
    pkg.writable_result(destination)
    pkg.require(not destination.exists(), "Scenario directory exists")
    source = Path(source) if source else (root.parent / "labs/lab6_1Hz_experiment.json" if laboratory else root / "stimulation/examples/shared.json")
    before = pkg.sha(source)
    c = pkg.probe(cli, source)["config"]
    original = copy.deepcopy(c)
    if laboratory:
        pkg.require(c["duration"] == 400 and len(c["neurons"]) == 500 and len(c["synapses"]) == 15041, "Unexpected laboratory Config; supply the matching initial experiment export")
        pkg.require(len(c["stimulators"]) == 1 and not c["stages"], "Expected one laboratory shared source and no stages")
        stim = copy.deepcopy(c["stimulators"][0])
        pkg.require(stim["signal"]["start"] == 150 and stim["first_delay"] == .05 and stim["signal"]["stop"] == 400, "Unexpected laboratory phase")
        background, end, interval = 150, 400, {"from_s": 280, "to_s": 400}
        profile = pkg.read_json(root / "experiments/analysis_profiles_20261020/lab6_profile.json")
    else:
        # Synthetic regression fixture, explicitly distinct from the laboratory.
        background, end, interval = .2, 1.4, {"from_s": .2, "to_s": 1.4}
        c.update(duration=end, noise_sigma=.2, history_capacity=0, bounded_journals=False)
        for n in c["neurons"]:
            n.update(amplitude=0, frequency=0, start=0, stop=0, pulse_intervals=[])
        c["stages"] = []
        stim = copy.deepcopy(c["stimulators"][0])
        stim.update(enabled=False, first_delay=.05)
        stim["signal"].update(start=background, stop=end, amplitude=30, frequency=1, width=.003, pulse_intervals=[])
        c["stimulators"] = [copy.deepcopy(stim)]
        profile = {"document_type": "network_studio_analysis_profile", "schema_version": 1, "name": "Short branch regression", "settings": c["analysis"]}
        profile["settings"].update(reference_stimulator=stim["id"], min_neurons=2, start_rate=100, end_rate=25, min_gap=.01, response_max=.06)
    destination.mkdir(parents=True)
    pkg.write_json(destination / "initial.json", c)
    pkg.write_json(destination / "profile.json", profile)
    branches = []
    for name, hz in (("unstimulated", 0), ("1hz", 1), ("10hz", 10)):
        value = copy.deepcopy(stim)
        value["enabled"] = hz > 0
        value["signal"]["frequency"] = hz or 1
        command = {"sequence": "1000001", "time": background, "restart": True, "value": value}
        pkg.write_json(destination / (name + ".json"), {"document_type": "network-studio-stimulator-commands", "schema": 1, "commands": [command], "changes": [], "latches": []})
        branches.append({"id": name, "commands": name + ".json"})
    spec = {"document_type": "network_studio_experiment_spec", "schema_version": 1, "name": "Laboratory common 150s, branches to 400s" if laboratory else "SHORT synthetic branches (not a laboratory reproduction)",
            "config": "initial.json", "checkpoint": None, "background_until_s": background, "until_s": end,
            "analysis_profile": "profile.json", "analysis_interval": interval, "threads": "auto" if laboratory else "2", "repeats": 2,
            "experiments": [{"id": f"seed-{s}", "noise_seed": s} for s in seeds], "branches": branches,
            "storage": {"mode": "archive", "rows": 2000 if laboratory else 128, "deliveries": False},
            "tolerances": {"spike_time_s": 1e-9, "stimulus_time_s": 1e-12, "weight_absolute": 1e-12, "weight_relative": 1e-10}}
    pkg.write_json(destination / "spec.json", spec)
    pkg.write_json(destination / "derivation.json", {"source": str(source.resolve()), "source_sha256": before,
        "scientific_config_changed": c != original, "changed_top_level_fields": [k for k in c if c[k] != original[k]],
        "note": "Synthetic shortened fixture" if not laboratory else "Scientific Config preserved; commands explicitly disable/change the one shared source at 150 s. Seed 42 matches input; other seeds are new experiments."})
    pkg.require(pkg.sha(source) == before, "Source changed")
    return destination / "spec.json"


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cli", type=Path, required=True)
    p.add_argument("--destination", type=Path, required=True)
    p.add_argument("--laboratory", action="store_true")
    p.add_argument("--source", type=Path)
    p.add_argument("--seeds", default="42,43", help="Distinct noise seeds; topology/generation seed stays fixed")
    args = p.parse_args()
    print(create(args.cli.resolve(), args.destination, args.laboratory, tuple(int(v) for v in args.seeds.split(",")), args.source))
