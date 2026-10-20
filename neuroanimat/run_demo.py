"""Sequential short demonstrations through the existing CLI; no numerical solver here."""
import argparse
import csv
import hashlib
import json
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--cli", type=Path, default=root / "build/Release/network_studio_cli.exe")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error("repeats must be 1..10")
    args.output.mkdir(parents=True, exist_ok=False)
    cli = args.cli.resolve()
    executable_hash = digest(cli)
    build = json.loads(subprocess.check_output([str(cli), "--package-build-info"], text=True))
    rows = []
    commands = []

    def run(name, options):
        if digest(cli) != executable_hash:
            raise RuntimeError("Build changed during the experiment")
        folder = args.output / name
        folder.mkdir()
        command = [str(cli), *map(str, options), "--threads", "1",
                   "--output", str(folder / "result"),
                   "--checkpoint", str(folder / "final.nscp")]
        start = time.perf_counter()
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   encoding="utf-8", errors="strict")
        elapsed = time.perf_counter() - start
        (folder / "console.txt").write_text(completed.stdout, encoding="utf-8")
        commands.append(command)
        if completed.returncode:
            raise RuntimeError(f"{name}: {completed.stdout}")
        compute = float(re.search(r"Wall seconds: ([\d.eE+-]+)", completed.stdout)[1])
        summary = json.loads((folder / "result_summary.json").read_text(encoding="utf-8"))
        analysis = json.loads((folder / "result_animat_analysis.json").read_text(encoding="utf-8"))
        row = dict(name=name, compute_seconds=compute, process_seconds=elapsed,
                   analysis=analysis, summary=summary,
                   checkpoint_sha256=digest(folder / "final.nscp"))
        rows.append(row)
        print(f"{name}: compute {compute:.6g} s; process {elapsed:.6g} s", flush=True)
        return folder

    for name, sequence in [("minimal", False), ("spatial_learning", True)]:
        config = args.output / (name + ".json")
        shutil.copy2(root / "neuroanimat/examples" / (name + ".json"), config)
        for repeat in range(args.repeats):
            run(f"{name}_{repeat + 1}", ["--config", config, *(["--sequence", "0"] if sequence else [])])
    # All branches share the exact same noise stream/state at 0.1 s. Changing
    # only zone frequencies is an environmental intervention, not a new seed.
    config = json.loads((args.output / "spatial_learning.json").read_text(encoding="utf-8"))
    config["stages"] = []
    config["duration"] = 1.4
    common_config = args.output / "branch_initial.json"
    common_config.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")
    common = run("common", ["--config", common_config, "--stop-at", ".1"])
    checkpoint = common / "final.nscp"
    before = digest(checkpoint)
    for frequency in (0, 1, 10):
        environment = json.loads(json.dumps(config["animat"]["environment"]))
        environment["enabled"] = frequency != 0
        environment["outside_frequency"] = frequency
        for zone in environment["zones"]:
            zone["frequency"] = frequency
        action = dict(sequence="1", time=.1, kind=0, stage=0, automatic=False,
                      learning=True, clear_traces=False, learning_rate=1,
                      environment=environment)
        protocol = args.output / f"branch_{frequency}Hz.json"
        protocol.write_text(json.dumps(dict(document_type="network-studio-animat-actions",
                                            schema=1, actions=[action]), indent=2), encoding="utf-8")
        for repeat in range(args.repeats):
            run(f"branch_{frequency}Hz_{repeat + 1}",
                ["--resume", checkpoint, "--animat-commands", protocol,
                 "--animat-from", ".1", "--animat-to", "1.4"])
        if digest(checkpoint) != before:
            raise RuntimeError("Common checkpoint was modified")
    # Matched learning control: identical saved state and unchanged 1/10 Hz
    # zones; only future STDP updates are enabled or frozen.
    for learning in (False, True):
        name = "learning_enabled" if learning else "learning_frozen"
        action = dict(sequence="1", time=.1, kind=3, stage=0, automatic=False,
                      learning=learning, clear_traces=False, learning_rate=1,
                      environment=config["animat"]["environment"])
        protocol = args.output / (name + ".json")
        protocol.write_text(json.dumps(dict(document_type="network-studio-animat-actions",
                                            schema=1, actions=[action]), indent=2), encoding="utf-8")
        for repeat in range(args.repeats):
            run(f"{name}_{repeat + 1}", ["--resume", checkpoint, "--animat-commands", protocol,
                                       "--animat-from", ".1", "--animat-to", "1.4"])
    # Replay the entire completed stage experiment, including stage entry.
    original = args.output / "spatial_learning_1"
    replay = run("replayed_stages", ["--config", original / "result_animat_initial.json",
                                   "--animat-commands", original / "result_animat_actions.json"])
    if digest(original / "final.nscp") != digest(replay / "final.nscp"):
        raise RuntimeError("Replayed stage experiment differs")
    checks = {}
    for group in ("minimal", "spatial_learning", "branch_0Hz", "branch_1Hz", "branch_10Hz", "learning_enabled", "learning_frozen"):
        selected = [r for r in rows if re.fullmatch(re.escape(group) + r"_\d+", r["name"])]
        values = [r["compute_seconds"] for r in selected]
        checkpoints = {r["checkpoint_sha256"] for r in selected}
        checks[group] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values),
                             bitwise_repeats=len(checkpoints) == 1, runs=len(values))
        if len(checkpoints) != 1:
            raise RuntimeError(f"{group}: repeat checkpoint mismatch")
    common_weights = json.loads((common / "result_summary.json").read_text())["final_synapse_weights"]
    frozen_weights = next(r for r in rows if r["name"] == "learning_frozen_1")["summary"]["final_synapse_weights"]
    if common_weights != frozen_weights:
        raise RuntimeError("Frozen STDP changed weights")
    for row in rows:
        with (args.output / row["name"] / "result_animat_zones.csv").open() as file:
            row["zones"] = list(csv.DictReader(file))
    inputs = {str(p.relative_to(args.output)): digest(p) for p in args.output.glob("*.json")}
    report = dict(build=build, executable_sha256=executable_hash, common_checkpoint_sha256=before,
                  input_sha256=inputs, commands=commands, runs=rows, timing=checks,
                  scope="Short examples only: 1 s minimal, 4 s staged, 0.1 + 1.3 s branches; no long laboratory validation")
    (args.output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    with (args.output / "timing.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["scenario", "repeats", "compute_median_s", "min_s", "max_s", "bitwise_repeats"])
        for name, item in checks.items():
            writer.writerow([name, item["runs"], item["median"], item["minimum"], item["maximum"], item["bitwise_repeats"]])
    print("All repeats and recorded stage replay are byte-exact.", flush=True)


if __name__ == "__main__":
    main()
