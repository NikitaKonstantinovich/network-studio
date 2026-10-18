"""Run the shipped examples and independently count activity from event CSVs.

No numerical packages or graphical backend are required. Results go to a new
directory; historical reports are not overwritten.
"""
import argparse
import csv
import datetime
import json
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or root / "build" / datetime.datetime.now().strftime("source-examples-%Y%m%d-%H%M%S-%f")
    output.mkdir(parents=True, exist_ok=False)
    results, reference = [], []
    for name in ["shared", "spatial", "chain", "fields_1hz", "fields_10hz"]:
        command = [str(root / "build/Release/network_studio_cli.exe"), "--config",
                   str(root / f"stimulation/examples/{name}.json"), "--output", str(output / name)]
        if name == "spatial":
            command += ["--stimulus-commands", str(root / "stimulation/examples/moves.json")]
        run = subprocess.run(command, check=True, capture_output=True, text=True)
        (output / f"{name}.log").write_text(run.stdout, encoding="utf-8")
        summary = json.loads((output / f"{name}_summary.json").read_text(encoding="utf-8"))

        def rows(suffix):
            with (output / f"{name}_{suffix}.csv").open(encoding="utf-8", newline="") as source:
                return list(csv.DictReader(source))

        deliveries, events, pulses = rows("deliveries"), rows("events"), rows("shared_stimuli")
        results.append(dict(example=name, time_s=summary["time_s"], spikes=summary["neuron_spikes"],
                            deliveries=len(deliveries), shared_pulses=len(pulses),
                            current_weights=summary["final_synapse_weights"]))
        if name not in ("chain", "fields_1hz", "fields_10hz"):
            continue
        end = summary["time_s"]
        posts = [(float(e["time_s"]), int(e["node_index_1based"])) for e in events if e["type"] == "spike"]
        row = dict(example=name, window_s=.05, response_s=[0, .02], time_s=end, edges=[])
        for edge, weight in enumerate(summary["final_synapse_weights"], 1):
            recent = [d for d in deliveries if int(d["synapse_index_1based"]) == edge
                      and end - .05 < float(d["t_arrival_s"]) <= end]
            answered = sum(any(node == int(d["target_index_1based"])
                               and 0 < t - float(d["t_arrival_s"]) <= .02 for t, node in posts)
                           for d in recent)
            row["edges"].append(dict(id=edge, deliveries_in_window=len(recent), deliveries_with_post=answered,
                                     q_arrivals=abs(weight) if recent else 0,
                                     q_with_post=abs(weight) if answered else 0))
        assert row["edges"][2]["deliveries_in_window"] > 0
        assert row["edges"][2]["deliveries_with_post"] == 0, "Silent last target acquired a spike"
        reference.append(row)
    for filename, data in [("example_results.json", results), ("activity_reference.json", reference)]:
        (output / filename).write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
