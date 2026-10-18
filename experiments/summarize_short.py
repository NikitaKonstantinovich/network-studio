"""Summarize completed short CLI runs; never infer long-run synchronization."""
import csv
import hashlib
import json
import math
import re
import statistics
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
work = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "verification/spatial_20261018/comparison_release"
output = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "experiments/short_comparison_release.json"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(name):
    return json.loads((work / name).read_text(encoding="utf-8-sig"))


def rows(name):
    with (work / name).open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


config = read("quick_1hz.json")
common = read("common_summary.json")
nodes, edges, space = config["neurons"], config["synapses"], config["space"]
degrees = [0] * len(nodes)
lengths = []
for edge in edges:
    degrees[edge["target"]] += 1
    a, b = nodes[edge["source"]], nodes[edge["target"]]
    lengths.append(math.hypot((a["px"]-b["px"])*space["width"], (a["py"]-b["py"])*space["height"]))
result = {
    "note": "Short transient only; the requested 100/200-second network runs were NOT completed.",
    "directory": str(work.resolve()),
    "neurons": len(nodes), "edges": len(edges), "space": space,
    "max_edge_length": max(lengths), "degree_mean": statistics.mean(degrees),
    "degree_sd": statistics.pstdev(degrees),
    "common_time_s": common["time_s"], "common_sha256": sha(work / "common.nscp"),
    "cli_sha256": sha(root / "build/Release/network_studio_cli.exe"),
    "cases": {},
}
for label in ("1hz", "10hz"):
    summary = read(label + "_summary.json")
    assert not summary["error"] and summary["complete"]
    analysis = dict(line.split("=", 1) for line in (work / (label + "_analysis.txt")).read_text().splitlines())
    pulse_times = sorted({float(row["time_s"]) for row in rows(label + "_stimuli.csv") if row["neuron_1based"] == "1"})
    result["cases"][label] = {
        "time_s": summary["time_s"], "stage_duration_s": summary["time_s"]-common["time_s"],
        "steps": summary["steps"], "total_spikes": sum(summary["neuron_spikes"]),
        "deliveries": summary["deliveries"],
        "changed_weights": sum(a != b for a, b in zip(common["final_synapse_weights"], summary["final_synapse_weights"])),
        "reference_pulses_s": pulse_times, "analysis": analysis,
        "bursts": rows(label + "_bursts.csv"),
        "wall_seconds": float(re.search(r"Wall seconds: ([0-9.]+)", (work / (label + ".log")).read_text()).group(1)),
        "checkpoint_sha256": sha(work / (label + ".nscp")),
    }
result["files"] = {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)} for p in sorted(work.iterdir()) if p.is_file()}
output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"output": str(output), "cases": {k: {key: v[key] for key in ("time_s", "total_spikes", "changed_weights", "wall_seconds")} for k, v in result["cases"].items()}}, ensure_ascii=False))
