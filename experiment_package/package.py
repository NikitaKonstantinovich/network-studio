"""Immutable experiment inputs, sequential fresh-process branches, audited comparisons.

Only Python's standard library is used. Scientific computation/validation belongs
to the packaged Network Studio executable; this module never integrates a model.
"""
from __future__ import annotations
import argparse
import copy
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

TYPE = "network_studio_experiment_package"
SCHEMA = 1
LIMIT_JSON = 64 * 1024 * 1024


def require(ok, message):
    if not ok:
        raise ValueError(message)


def read_json(path, limit=LIMIT_JSON):
    path = Path(path)
    require(path.stat().st_size <= limit, f"JSON exceeds limit: {path}")
    def pairs(items):
        result = {}
        for k, v in items:
            require(k not in result, f"Duplicate JSON key: {k}")
            result[k] = v
        return result
    def bad(v):
        raise ValueError(f"Non-finite JSON number: {v}")
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs, parse_constant=bad)


def write_json(path, data):
    path = Path(path)
    # Reports are atomically updated; immutable input destinations are checked separately.
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("x", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2, allow_nan=False)
        f.write("\n")
    os.replace(tmp, path)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def identifier(s):
    require(isinstance(s, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", s), "Invalid experiment/branch ID")
    require(s.upper().split(".")[0] not in {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(10)], *[f"LPT{i}" for i in range(10)]}, "Reserved path ID")
    return s


def finite(x, lo=0):
    require(type(x) in (int, float) and math.isfinite(x) and x >= lo, "Expected finite nonnegative number")
    return x


def writable_result(path):
    # This project's historical laboratory directory is explicitly read-only.
    labs = Path("E:/asp/26_27/labs").resolve()
    require(not Path(path).resolve().is_relative_to(labs), "The historical labs directory is read-only; choose a separate output directory")


def inside(root, relative):
    require(isinstance(relative, str) and not Path(relative).is_absolute(), "Package paths must be relative")
    root = Path(root).resolve()
    path = (root / relative).resolve()
    require(path.is_relative_to(root) and path != root, "Path escapes package")
    return path


def cli_call(cli, args, log=None):
    command = [str(cli), *map(str, args)]
    start = time.perf_counter()
    if log:
        with Path(log).open("x", encoding="utf-8") as f:
            result = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, encoding="utf-8", check=False)
        require(result.returncode == 0, f"CLI exit {result.returncode}; see {log}")
        return time.perf_counter() - start
    result = subprocess.run(command, capture_output=True, encoding="utf-8", check=False)
    require(result.returncode == 0, f"CLI exit {result.returncode}: {result.stderr[-6000:]} {result.stdout[-2000:]}")
    return json.loads(result.stdout)


def probe(cli, source, checkpoint=False, **options):
    args = ["--package-probe", "--resume" if checkpoint else "--config", source]
    for k, v in options.items():
        args += ["--" + k.replace("_", "-"), v]
    return cli_call(cli, args)


def check_spec(s):
    require(s.get("document_type") == "network_studio_experiment_spec" and s.get("schema_version") == 1, "Unsupported experiment specification")
    a, b, end = finite(s["analysis_interval"]["from_s"]), finite(s["background_until_s"]), finite(s["until_s"])
    z = finite(s["analysis_interval"]["to_s"])
    require(0 <= b < end and 0 <= a < z <= end, "Invalid background/end/analysis interval")
    require(str(s["threads"]) in ("1", "2", "4", "6", "8", "12", "16", "auto"), "Invalid threads")
    require(type(s["repeats"]) is int and 1 <= s["repeats"] <= 100, "Repeats must be 1..100")
    require(1 <= len(s["experiments"]) <= 100 and 1 <= len(s["branches"]) <= 32, "Too many/few experiments or branches")
    require(len(s["experiments"]) * len(s["branches"]) * s["repeats"] <= 1000, "At most 1000 branch runs per package")
    for group in (s["experiments"], s["branches"]):
        ids = [identifier(v["id"]).lower() for v in group]
        require(len(ids) == len(set(ids)), "Duplicate IDs (case-insensitive)")
    for e in s["experiments"]:
        require(type(e["noise_seed"]) is int and 0 <= e["noise_seed"] <= 0xffffffff, "Noise seed must be uint32")
    require(len({e["noise_seed"] for e in s["experiments"]}) == len(s["experiments"]), "Use repeats for the same seed; seed-series experiments must have distinct seeds")
    storage = s["storage"]
    require(storage["mode"] in ("archive", "memory"), "Storage mode: archive or memory")
    require(type(storage["rows"]) is int and 2 <= storage["rows"] <= 1000000, "Archive rows must be 2..1000000")
    require(type(storage["deliveries"]) is bool, "deliveries must be boolean")
    require(set(s["tolerances"]) == {"spike_time_s", "stimulus_time_s", "weight_absolute", "weight_relative"}, "Specify all four comparison tolerances")
    for v in s["tolerances"].values():
        finite(v)


def check_commands(doc, start, end):
    require(doc.get("document_type") == "network-studio-stimulator-commands" and doc.get("schema") == 1, "Unsupported command journal")
    require(doc.get("changes") == [] and not doc.get("latches"), "Package protocols use commands only; flatten accepted changes into commands explicitly")
    require(isinstance(doc.get("commands"), list) and len(doc["commands"]) <= 4096, "Invalid command list")
    seen = set()
    previous = (-1, -1)
    for command in doc["commands"]:
        t = finite(command["time"])
        seq = command["sequence"]
        require(isinstance(seq, str) and re.fullmatch(r"[1-9][0-9]{0,19}", seq) and int(seq) <= 2**64-1, "Invalid command sequence")
        key = (t, int(seq))
        require(start <= t < end and seq not in seen and key > previous, "Commands must be ordered, unique, and inside [background,end)")
        seen.add(seq)
        previous = key
        require(not command["value"]["spatial"], "Package protocols require explicit recipients; resolve the spatial selection first")
    return doc["commands"]


def accepted_protocol(doc):
    """Flatten an accepted GUI/CLI journal without rescheduling its absolute times."""
    require(doc.get("document_type") == "network-studio-stimulator-commands" and doc.get("schema") == 1, "Unsupported command journal")
    commands = copy.deepcopy(doc["commands"])
    for change in doc["changes"]:
        command = {k: copy.deepcopy(change[k]) for k in ("sequence", "time", "restart", "value")}
        if command["value"]["spatial"]:
            # Use the recipients recorded at acceptance, never current geometry.
            command["value"].update(spatial=False, recipients=copy.deepcopy(change["recipients"]))
        commands.append(command)
    commands.sort(key=lambda v: (v["time"], int(v["sequence"])))
    return {"document_type": "network-studio-stimulator-commands", "schema": 1, "commands": commands, "changes": [], "latches": []}


def generations(config):
    return [{"first": g["first"], "count": g["count"], "settings": json.loads(g["settings_json"])} for g in config["generation"]]


def prepare(spec_path, destination, cli):
    spec_path, destination, cli = Path(spec_path).resolve(), Path(destination).resolve(), Path(cli).resolve()
    writable_result(destination)
    s = read_json(spec_path)
    check_spec(s)
    require(not destination.exists() and destination.parent.is_dir(), "Package destination must be new, with an existing parent")
    base = spec_path.parent
    source = (base / s["config"]).resolve()
    cp = (base / s["checkpoint"]).resolve() if s.get("checkpoint") else None
    build = cli_call(cli, ["--package-build-info"])
    initial = probe(cli, source)
    config = initial["config"]
    require(config["duration"] == s["until_s"], "Config.duration must equal until_s; change the protocol specification explicitly")
    require(s["background_until_s"] == round(s["background_until_s"] / config["dt"]) * config["dt"], "Background time must be an exact macro-step boundary (N * dt)")
    if cp:
        restored = probe(cli, cp, True)
        require(not restored["archive_linked"], "Imported NSCP references an external archive. Schema 1 accepts standalone checkpoints only; start this package from Config to record its complete archive lineage")
        require(restored["time_s"] == s["background_until_s"], "Imported checkpoint must be exactly at the branch boundary")
        require(restored["config"] == config, "Input Config differs from checkpoint Config; export its original Config, not learned weights")
        require(len(s["experiments"]) == 1 and s["experiments"][0]["noise_seed"] == config["seed"], "Checkpoint seed is immutable; a seed series requires fresh background experiments")
        require(s["storage"]["mode"] == "memory", "A standalone progressed checkpoint cannot attach a disk archive retroactively")
    profile_source = (base / s["analysis_profile"]).resolve()
    read_json(profile_source, 65536)
    # Inputs are copied byte-for-byte. Derived Config changes are explicit metadata.
    destination.mkdir()
    (destination / "inputs").mkdir()
    (destination / "inputs" / "experiments").mkdir()
    (destination / "inputs" / "protocols").mkdir()
    (destination / "inputs" / "protocol-sources").mkdir()
    (destination / "runtime").mkdir()
    shutil.copy2(cli, destination / "runtime" / cli.name)
    shutil.copy2(Path(__file__), destination / "runtime" / "package.py")
    shutil.copy2(source, destination / "inputs" / "source.json")
    shutil.copy2(spec_path, destination / "inputs" / "source_spec.json")
    shutil.copy2(profile_source, destination / "inputs" / "profile.json")
    if cp:
        shutil.copy2(cp, destination / "inputs" / "source.nscp")
    m = copy.deepcopy(s)
    m.update(document_type=TYPE, schema_version=SCHEMA, executable="runtime/" + cli.name,
             runner="runtime/package.py", build=build, config="inputs/source.json",
             checkpoint="inputs/source.nscp" if cp else None, analysis_profile="inputs/profile.json",
             created_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
             environment={"platform": platform.platform(), "python": platform.python_version(), "machine": platform.machine(),
                          "processor": os.environ.get("PROCESSOR_IDENTIFIER", platform.processor())},
             source_provenance={"config": str(source), "checkpoint": str(cp) if cp else None, "spec": str(spec_path)})
    for e in m["experiments"]:
        c = copy.deepcopy(config)
        c["seed"] = e["noise_seed"]
        if s["storage"]["mode"] == "archive":
            c.update(history_capacity=s["storage"]["rows"], bounded_journals=True, journal_capacity=50000)
        else:
            require(not c["bounded_journals"] and c["history_capacity"] == 0, "Memory packages require full untrimmed journals/history; use archive mode for bounded RAM")
        e["config"] = f"inputs/experiments/{e['id']}.json"
        e["new_experiment"] = not bool(cp)
        e["generation"] = generations(c)
        e["config_overrides"] = {k: c[k] for k in c if c[k] != config[k]}
        e["seed_semantics"] = "fresh noise realization; fixed topology" if not cp else "unchanged checkpoint seed"
        write_json(destination / e["config"], c)
    for branch in m["branches"]:
        src = (base / branch["commands"]).resolve()
        protocol = accepted_protocol(read_json(src, 16 * 1024 * 1024))
        check_commands(protocol, s["background_until_s"], s["until_s"])
        branch["commands"] = f"inputs/protocols/{branch['id']}.json"
        branch["source_commands"] = f"inputs/protocol-sources/{branch['id']}.json"
        shutil.copy2(src, destination / branch["source_commands"])
        write_json(destination / branch["commands"], protocol)
    m["files"] = {p.relative_to(destination).as_posix(): {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(destination.rglob("*")) if p.is_file()}
    write_json(destination / "manifest.json", m)
    result = validate(destination)
    write_json(destination / "preflight.json", result)
    print(f"Created package: {destination}", flush=True)
    return m


def validate(package, output=None):
    package = Path(package).resolve()
    m = read_json(package / "manifest.json")
    require(m.get("document_type") == TYPE and m.get("schema_version") == SCHEMA, "Unsupported package")
    spec = dict(m, document_type="network_studio_experiment_spec")
    check_spec(spec)
    require(1 <= len(m["files"]) <= 512, "Package file count limit")
    for rel, record in m["files"].items():
        p = inside(package, rel)
        require(p.is_file() and p.stat().st_size == record["bytes"] and sha(p) == record["sha256"], f"Missing/changed input: {rel}")
    def input_file(rel):
        require(rel in m["files"], f"Unhashed input: {rel}")
        return inside(package, rel)
    cli = input_file(m["executable"])
    require(cli_call(cli, ["--package-build-info"]) == m["build"], "Build/hardware identity differs; exact reproduction is not certified on this environment")
    profile = input_file(m["analysis_profile"])
    settings = read_json(profile)["settings"]
    require((m["analysis_interval"]["to_s"] - m["analysis_interval"]["from_s"]) / settings["bin"] <= 200000, "Analysis exceeds 200000 bins")
    source = input_file(m["checkpoint"]) if m["checkpoint"] else None
    original = probe(cli, input_file(m["config"]))["config"]
    rows = []
    for e in m["experiments"]:
        config_path = input_file(e["config"])
        initial = probe(cli, source or config_path, bool(source), threads=m["threads"])
        c = initial["config"]
        expected = copy.deepcopy(original)
        expected["seed"] = e["noise_seed"]
        if m["storage"]["mode"] == "archive":
            expected.update(history_capacity=m["storage"]["rows"], bounded_journals=True, journal_capacity=50000)
        else:
            require(not expected["bounded_journals"] and expected["history_capacity"] == 0, "Memory recording must retain full history")
        require(c == expected and e["config_overrides"] == {k: expected[k] for k in expected if expected[k] != original[k]}, "Derived Config changes exceed the declared seed/recording policy")
        require(c["seed"] == e["noise_seed"] and generations(c) == e["generation"], "Seed/generation metadata mismatch")
        require(c["duration"] == m["until_s"], "End time differs from configuration")
        require(not initial["stage_active"] and not initial["runtime"]["automatic"], "Package cannot silently replace an active/automatic stage")
        require(not initial["stimulator_runtime"]["commands"], "Input already has pending commands; include them explicitly in each branch protocol")
        require(not initial["archive_linked"], "Imported archive-linked NSCP is not portable in package schema 1")
        require(max(initial[k] for k in ("events_after_s", "stimuli_after_s", "shared_after_s")) == 0, "Incomplete detector prehistory in input")
        if source:
            require(c == probe(cli, config_path)["config"] and initial["time_s"] == m["background_until_s"], "Checkpoint/config/time mismatch")
        else:
            require(initial["time_s"] == 0, "Fresh experiment must start at zero")
        for b in m["branches"]:
            commands = input_file(b["commands"])
            require(read_json(commands) == accepted_protocol(read_json(input_file(b["source_commands"]))), "Accepted protocol differs from its source journal")
            check_commands(read_json(commands), m["background_until_s"], m["until_s"])
            probe(cli, source or config_path, bool(source), commands=commands, profile=profile, threads=m["threads"])
        rows.append({"experiment": e["id"], "noise_seed": e["noise_seed"], "threads_requested": initial["threads_requested"], "threads_effective": initial["threads_effective"]})
    if output is not None:
        out = Path(output).resolve()
        writable_result(out)
        require(not out.exists() and out.parent.is_dir(), "Output directory must be new; existing runs are never overwritten")
        require(not out.is_relative_to(package), "Keep output outside the immutable input package")
    return {"status": "valid", "simulation_steps": 0, "manifest_sha256": sha(package / "manifest.json"), "runs": rows,
            "analysis": {"requested": m["analysis_interval"], "available_now_until_s": m["background_until_s"] if source else 0,
                         "planned_until_s": m["until_s"], "future_coverage": "conditional on successful completion; checked again after each run"}}


def json_lines(path):
    with Path(path).open(encoding="utf-8") as f:
        for line in f:
            yield json.loads(line)


def split_records(raw, directory):
    kinds = ("spike", "release", "stimulus", "pulse")
    counts = dict.fromkeys(kinds, 0)
    onsets = {}
    files = {k: (directory / (k + ".jsonl")).open("x", encoding="utf-8", newline="\n") for k in kinds}
    try:
        for r in json_lines(raw):
            kind = r[0]
            require(kind in files, "Unknown comparison record")
            counts[kind] += 1
            files[kind].write(json.dumps(r, separators=(",", ":"), allow_nan=False) + "\n")
            if kind == "pulse" and not r[7]:
                entry = onsets.setdefault(str(r[2]), {"count": 0, "first_s": r[4], "last_s": r[4]})
                entry["count"] += 1
                entry["last_s"] = r[4]
    finally:
        for f in files.values():
            f.close()
    return {"counts": counts, "shared_onsets": onsets, "sha256": {k: sha(directory / (k + ".jsonl")) for k in kinds}}


def memory_records(prefix, destination, start):
    """Normalize full RAM exports. Interval is [branch boundary, final time]."""
    def rows(suffix):
        with Path(str(prefix) + suffix).open(encoding="utf-8-sig", newline="") as f:
            yield from csv.DictReader(f)
    with destination.open("x", encoding="utf-8", newline="\n") as f:
        def put(r):
            f.write(json.dumps(r, allow_nan=False) + "\n")
        for e in rows("_events.csv"):
            t = float(e["time_s"])
            # Events at the checkpoint boundary already belong to the common prefix.
            if t > start:
                put(["spike" if e["type"] == "spike" else "release", t, int(e["node_index_1based"]) - 1])
        for e in rows("_stimuli.csv"):
            t = float(e["time_s"])
            if t > start:
                put(["stimulus", t, float(e["amplitude"]), int(e["neuron_1based"]) - 1, int(e["protocol_0based"])])
        for p in rows("_shared_stimuli.csv"):
            t = float(p["start_s"])
            if t < start:
                continue
            rec = [int(v) - 1 for v in p["recipients_1based"].split(";") if v]
            common = [int(p["stimulator_id"]), int(p["pulse_id"])]
            tail = [float(p["amplitude"]), 0, int(p["stage_0based"]) + 1, rec]
            put(["pulse", t, *common, t, t, *tail])
            if int(p["complete"]):
                end = float(p["end_s"])
                tail[1] = 1
                put(["pulse", end, *common, t, end, *tail])


def compare_stream(a, b, tolerance, time_indices):
    exact = sha(a) == sha(b)
    same_identity = True
    maximum = 0.0
    count_a = count_b = 0
    within = True
    for x, y in itertools.zip_longest(json_lines(a), json_lines(b)):
        count_a += x is not None
        count_b += y is not None
        if x is None or y is None:
            same_identity = within = False
            continue
        require(len(x) == len(y), "Comparison record schema mismatch")
        for i, (u, v) in enumerate(zip(x, y)):
            if i in time_indices:
                maximum = max(maximum, abs(u - v))
                within &= abs(u - v) <= tolerance
            elif u != v:
                same_identity = within = False
    return {"exact": exact, "within_tolerance": within, "same_count_order_identity": same_identity,
            "count_a": count_a, "count_b": count_b, "max_time_error_s": maximum, "absolute_tolerance_s": tolerance}


def compare_weights(a, b, absolute, relative):
    require(len(a) == len(b), "Weights have different topology dimensions")
    errors = [abs(x - y) for x, y in zip(a, b)]
    return {"exact": a == b, "within_tolerance": all(d <= absolute + relative * max(abs(x), abs(y)) for x, y, d in zip(a, b, errors)),
            "count": len(a), "changed": sum(x != y for x, y in zip(a, b)), "max_absolute_error": max(errors, default=0),
            "absolute_tolerance": absolute, "relative_tolerance": relative}


def compare_runs(root, a, b, tolerances):
    pa, pb = root / a["path"], root / b["path"]
    same_input = a["parent_sha256"] == b["parent_sha256"]
    same_protocol = a["protocol_sha256"] == b["protocol_sha256"]
    result = {"a": a["path"], "b": b["path"], "same_parent_checkpoint": same_input, "same_protocol": same_protocol,
              "same_analysis_profile_interval": a["analysis_identity"] == b["analysis_identity"],
              "exact_normalized_checkpoint": sha(pa / "comparison.nscp") == sha(pb / "comparison.nscp"),
              "interpretation": "deterministic repeat" if same_input and same_protocol else "different branch conditions; equality is not expected"}
    require(a["topology_sha256"] == b["topology_sha256"], "Incompatible topology; refusing index-based weight comparison")
    for kind, indices in (("spike", (1,)), ("release", (1,)), ("stimulus", (1,)), ("pulse", (1, 4, 5))):
        result[kind] = compare_stream(pa / (kind + ".jsonl"), pb / (kind + ".jsonl"), tolerances["spike_time_s" if kind in ("spike", "release") else "stimulus_time_s"], indices)
    result["weights"] = compare_weights(read_json(pa / "result_summary.json")["final_synapse_weights"], read_json(pb / "result_summary.json")["final_synapse_weights"], tolerances["weight_absolute"], tolerances["weight_relative"])
    return result


def spread(values):
    return {"n": len(values), "values": values, "mean": statistics.mean(values), "median": statistics.median(values),
            "minimum": min(values), "maximum": max(values), "sample_sd": statistics.stdev(values) if len(values) > 1 else None}


def run(package, output):
    package, output = Path(package).resolve(), Path(output).resolve()
    preflight = validate(package, output)
    m = read_json(package / "manifest.json")
    cli = inside(package, m["executable"])
    output.mkdir()
    write_json(output / "preflight.json", preflight)
    shutil.copy2(package / "manifest.json", output / "input_manifest.json")
    report = {"document_type": "network_studio_experiment_report", "schema_version": 1, "status": "running", "package": str(package),
              "manifest_sha256": preflight["manifest_sha256"], "build": m["build"], "tolerances": m["tolerances"],
              "model_interval_s": [0, m["until_s"]], "analysis_interval": m["analysis_interval"], "runs": [], "comparisons": [], "seed_series": {}}
    started = time.perf_counter()
    archived = m["storage"]["mode"] == "archive"
    def unchanged_inputs():
        require(sha(package / "manifest.json") == report["manifest_sha256"], "Package manifest changed during execution")
        for name, entry in m["files"].items():
            require(sha(inside(package, name)) == entry["sha256"], f"Package input changed during execution: {name}")
    def archive_args(directory):
        return ["--archive", directory / "history", "--archive-rows", m["storage"]["rows"], "--archive-deliveries", "yes" if m["storage"]["deliveries"] else "no"] if archived else []
    try:
        for e in m["experiments"]:
            unchanged_inputs()
            common = output / e["id"] / "common"
            common.mkdir(parents=True)
            cp = common / "background.nscp"
            if m["checkpoint"]:
                shutil.copy2(inside(package, m["checkpoint"]), cp)
                background_seconds = 0
            else:
                print(f"Background {e['id']}: 0 -> {m['background_until_s']} s", flush=True)
                background_seconds = cli_call(cli, ["--config", inside(package, e["config"]), "--stop-at", m["background_until_s"],
                    "--threads", m["threads"], "--checkpoint", cp, "--output", common / "result", *archive_args(common)], common / "run.log")
            parent_hash = sha(cp)
            parent = probe(cli, cp, True)
            require(parent["time_s"] == m["background_until_s"], "Background stopped at a different boundary; protocol was not shifted automatically")
            topology = {k: parent["config"][k] for k in ("neurons", "synapses", "astrocytes", "gaps")}
            topology_hash = hashlib.sha256(json.dumps(topology, sort_keys=True, allow_nan=False).encode()).hexdigest()
            write_json(common / "provenance.json", {"parent_sha256": parent_hash, "source_config_sha256": sha(inside(package, e["config"])),
                "input_checkpoint_sha256": sha(inside(package, m["checkpoint"])) if m["checkpoint"] else None, "seed": e["noise_seed"],
                "generation": e["generation"], "time_s": parent["time_s"], "background_wall_s": background_seconds})
            # Validate again against the ACTUAL common state before launching any branch.
            for b in m["branches"]:
                probe(cli, cp, True, commands=inside(package, b["commands"]), profile=inside(package, m["analysis_profile"]), threads=m["threads"])
            for b in m["branches"]:
                for repeat in range(1, m["repeats"] + 1):
                    unchanged_inputs()
                    require(sha(cp) == parent_hash, "Common checkpoint changed")
                    rel = f"{e['id']}/{b['id']}/repeat-{repeat:03d}"
                    d = output / rel
                    d.mkdir(parents=True)
                    print(f"Branch {rel}: {parent['time_s']} -> {m['until_s']} s", flush=True)
                    row = {"path": rel, "experiment": e["id"], "branch": b["id"], "repeat": repeat, "noise_seed": e["noise_seed"],
                           "parent_sha256": parent_hash, "protocol_sha256": sha(inside(package, b["commands"])), "topology_sha256": topology_hash,
                           "analysis_identity": {"profile_sha256": sha(inside(package, m["analysis_profile"])), "interval": m["analysis_interval"]}}
                    write_json(d / "provenance.json", row)
                    row["simulation_wall_s"] = cli_call(cli, ["--resume", cp, "--until", m["until_s"], "--threads", m["threads"],
                        "--stimulus-commands", inside(package, b["commands"]), "--checkpoint", d / "final.nscp", "--output", d / "result", *archive_args(d)], d / "run.log")
                    final = probe(cli, d / "final.nscp", True, canonical_state=d / "comparison.nscp", threads=m["threads"])
                    require(final["time_s"] == m["until_s"], "Run did not reach the requested end")
                    require(not final["stimulator_runtime"]["commands"], "Unapplied commands remain")
                    expected = read_json(inside(package, b["commands"]))["commands"]
                    actual = {x["sequence"]: x for x in final["stimulator_runtime"]["changes"]}
                    row["commands_exact"] = all(all(actual.get(c["sequence"], {}).get(k) == c[k] for k in ("time", "restart", "value")) and actual[c["sequence"]]["recipients"] == sorted(c["value"]["recipients"]) for c in expected)
                    require(row["commands_exact"], "Applied commands differ from the accepted protocol")
                    row.update(threads_requested=final["threads_requested"], threads_effective=final["threads_effective"], final_time_s=final["time_s"])
                    write_json(d / "accepted_protocol.json", final["stimulator_runtime"])
                    if archived:
                        cli_call(cli, ["--package-records", "--archive", d / "history", "--output", d / "records.jsonl"], d / "records.log")
                    else:
                        require(max(final[k] for k in ("events_after_s", "stimuli_after_s", "shared_after_s")) == 0, "History was truncated")
                        memory_records(d / "result", d / "records.jsonl", parent["time_s"])
                    row["actual_records"] = split_records(d / "records.jsonl", d)
                    interval = m["analysis_interval"]
                    cli_call(cli, ["--analyze-archive" if archived else "--analyze-export", d / ("history" if archived else "result"),
                        "--analysis-profile", inside(package, m["analysis_profile"]), "--analysis-from", interval["from_s"], "--analysis-to", interval["to_s"],
                        "--output", d / "analysis"], d / "analysis.log")
                    analysis = read_json(d / "analysis_analysis.json")
                    require(analysis["full_interval"] and analysis["prehistory_complete"], "Analysis interval/prehistory is incomplete; results are not certified")
                    row["analysis"] = analysis
                    weights = read_json(d / "result_summary.json")["final_synapse_weights"]
                    row["metrics"] = {"spikes": row["actual_records"]["counts"]["spike"], "complete_bursts": analysis["complete_bursts"],
                                      "response_fraction": analysis["response_fraction"], "mean_final_weight": statistics.mean(weights) if weights else None}
                    row["output_sha256"] = {p.name: sha(p) for p in d.iterdir() if p.is_file() and p.name != "provenance.json"}
                    report["runs"].append(row)
                    unchanged_inputs()
                    write_json(output / "report.json", report)
            require(sha(cp) == parent_hash, "Common checkpoint was changed during branches")
        for e in m["experiments"]:
            runs = [r for r in report["runs"] if r["experiment"] == e["id"]]
            for b in m["branches"]:
                repeats = [r for r in runs if r["branch"] == b["id"]]
                for r in repeats[1:]:
                    comparison = compare_runs(output, repeats[0], r, m["tolerances"])
                    report["comparisons"].append(comparison)
                    require(comparison["exact_normalized_checkpoint"] and all(comparison[k]["exact"] for k in ("spike", "release", "stimulus", "pulse", "weights")), "Deterministic repeat is not exact; tolerances do not excuse a repeat failure")
            firsts = [r for r in runs if r["repeat"] == 1]
            for a, b in itertools.combinations(firsts, 2):
                report["comparisons"].append(compare_runs(output, a, b, m["tolerances"]))
        for b in m["branches"]:
            runs = [r for r in report["runs"] if r["branch"] == b["id"] and r["repeat"] == 1]
            # Repeats do not inflate n: one observation per experiment/seed.
            report["seed_series"][b["id"]] = {"experiment_ids": [r["experiment"] for r in runs], "noise_seeds": [r["noise_seed"] for r in runs],
                "metrics": {key: spread([r["metrics"][key] for r in runs]) for key in runs[0]["metrics"] if all(r["metrics"][key] is not None for r in runs)}}
        report["status"] = "complete"
    except BaseException as exc:
        report.update(status="failed", diagnostic=str(exc))
        raise
    finally:
        report["wall_s"] = time.perf_counter() - started
        write_json(output / "report.json", report)
        render_report(output, report)
    print(f"Complete: {output / 'REPORT.md'}", flush=True)
    return report


def render_report(output, r):
    lines = ["# Experiment package report", "", f"Status: **{r['status']}**. Wall time: {r['wall_s']:.3f} s.",
             f"Model interval per fresh experiment: {r['model_interval_s']} s; comparison: {r['analysis_interval']}.",
             "", "This report describes only the durations actually executed. It is not evidence of a longer laboratory run.",
             "", "| Run | Seed | Threads requested/effective | Spikes (branch) | Complete bursts | Mean final weight | Wall s |", "|---|---:|---|---:|---:|---:|---:|"]
    for row in r["runs"]:
        v = row["metrics"]
        lines.append(f"| {row['path']} | {row['noise_seed']} | {row['threads_requested']}/{row['threads_effective']} | {v['spikes']} | {v['complete_bursts']} | {v['mean_final_weight']} | {row['simulation_wall_s']:.3f} |")
    lines += ["", "| Comparison | Same starting NSCP | Same protocol | Exact state NSCP* | Exact spikes | Spike tolerance | Exact weights | Weight tolerance |", "|---|---|---|---|---|---|---|---|"]
    for c in r["comparisons"]:
        lines.append(f"| {c['a']} / {c['b']} | {c['same_parent_checkpoint']} | {c['same_protocol']} | {c['exact_normalized_checkpoint']} | {c['spike']['exact']} | {c['spike']['within_tolerance']} | {c['weights']['exact']} | {c['weights']['within_tolerance']} |")
    lines += ["", "*Comparison NSCP removes only the archive location/ID after strict loading; all solver state, queues, traces, counters and retained histories remain. Full post-boundary spikes and stimuli are compared separately from the disk archive.",
              "Different protocols are expected to differ. Tolerances never turn a failed deterministic repeat into a pass.",
              "", "## Seed series (one observation per seed experiment, not per repeat)"]
    for name, series in r["seed_series"].items():
        lines.append(f"\n{name}: seeds {series['noise_seeds']}")
        for key, stats in series["metrics"].items():
            lines.append(f"- {key}: values={stats['values']}; mean={stats['mean']}; sample SD={stats['sample_sd']}; range=[{stats['minimum']}, {stats['maximum']}]")
    lines += ["", "Detailed identities, applied commands, actual pulse/event records, coverage and SHA-256 are in report.json and the run subdirectories."]
    if r.get("diagnostic"):
        lines.append("\nFailure: " + r["diagnostic"])
    (output / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    create = sub.add_parser("create", help="Copy and hash immutable inputs + CLI; validate without simulation")
    create.add_argument("--spec", required=True)
    create.add_argument("--destination", required=True)
    create.add_argument("--cli", required=True)
    for name in ("validate", "run"):
        p = sub.add_parser(name)
        p.add_argument("--package", required=True)
        p.add_argument("--output", required=name == "run")
    args = parser.parse_args()
    try:
        if args.action == "create":
            prepare(args.spec, args.destination, args.cli)
        elif args.action == "validate":
            print(json.dumps(validate(args.package, args.output), ensure_ascii=False, indent=2))
        else:
            run(args.package, args.output)
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"Experiment package: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
