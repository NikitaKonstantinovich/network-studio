"""Fresh-process package regression. All artifacts go into a new timestamped directory."""
import argparse
import copy
import datetime
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import package as p
import make_scenario


def main(cli, base):
    start = time.perf_counter()
    work = base / datetime.datetime.now().strftime("test_%Y%m%d_%H%M%S_%f")
    work.mkdir(parents=True)
    passed = []
    def check(ok, name):
        p.require(ok, name)
        passed.append(name)
    def rejects(fn, name, expected=None):
        try:
            fn()
        except (ValueError, OSError) as exc:
            if expected:
                check(expected in str(exc), name + " diagnostic")
            passed.append(name)
            return
        raise AssertionError("Unexpected success: " + name)
    spec = make_scenario.create(cli, work / "исходные данные")
    bundle = work / "пакет"
    p.prepare(spec, bundle, cli)
    check(p.validate(bundle)["simulation_steps"] == 0, "preflight performs no steps")
    run = work / "ветви"
    r = p.run(bundle, run)
    check(r["status"] == "complete" and len(r["runs"]) == 12, "two seeds, three branches, two fresh-process repeats")
    check(len({v["parent_sha256"] for v in r["runs"]}) == 2, "one common state per seed")
    for row in r["runs"]:
        d = run / row["path"]
        onsets = [v for v in p.json_lines(d / "pulse.jsonl") if not v[7]]
        hz = {"unstimulated": 0, "1hz": 1, "10hz": 10}[row["branch"]]
        expected = [] if hz == 0 else [.25 + k / hz for k in range(2 if hz == 1 else 12)]
        check(len(onsets) == len(expected), row["path"] + " pulse count")
        check(all(abs(v[4] - t) <= 2e-15 and v[9] == list(range(6)) and v[6] == 30 for v, t in zip(onsets, expected)), row["path"] + " independent pulse times/recipients/amplitude")
        check(row["commands_exact"], row["path"] + " command application")
        check(row["analysis"]["full_interval"] and row["analysis"]["prehistory_complete"], row["path"] + " full analysis with prehistory")
    check(all(c["exact_normalized_checkpoint"] for c in r["comparisons"] if c["same_protocol"]), "strict state/queues/traces/history repeat equality")
    check(all(v["metrics"]["spikes"]["n"] == 2 for v in r["seed_series"].values()), "repeats do not inflate seed sample size")
    check(r["seed_series"]["1hz"]["metrics"]["mean_final_weight"]["sample_sd"] > 0, "noise-seed series changes outcome")
    # Compare branching with uninterrupted integration under the identical accepted protocol.
    control = work / "continuous"
    control.mkdir()
    m = p.read_json(bundle / "manifest.json")
    p.cli_call(cli, ["--config", bundle / m["experiments"][0]["config"], "--stimulus-commands", bundle / m["branches"][1]["commands"],
        "--threads", "1", "--archive", control / "history", "--archive-rows", "128", "--checkpoint", control / "final.nscp", "--output", control / "result"], control / "run.log")
    p.probe(cli, control / "final.nscp", True, canonical_state=control / "comparison.nscp")
    check(p.sha(control / "comparison.nscp") == p.sha(run / "seed-42/1hz/repeat-001/comparison.nscp"), "continuous equals checkpoint branch, including solver state and history")
    # A standalone NSCP is a first-class package input; no seed rewrite after resume.
    memory = work / "memory-input"
    memory.mkdir()
    cfg = p.read_json(spec.parent / "initial.json")
    p.write_json(memory / "config.json", cfg)
    p.cli_call(cli, ["--config", memory / "config.json", "--stop-at", ".2", "--checkpoint", memory / "common.nscp", "--output", memory / "prefix"], memory / "run.log")
    ms = p.read_json(spec)
    ms.update(config=str(memory / "config.json"), checkpoint=str(memory / "common.nscp"), analysis_profile=str(spec.parent / "profile.json"), experiments=[ms["experiments"][0]])
    ms["storage"]["mode"] = "memory"
    for b in ms["branches"]:
        b["commands"] = str(spec.parent / b["commands"])
    p.write_json(memory / "spec.json", ms)
    p.prepare(memory / "spec.json", work / "checkpoint-package", cli)
    rm = p.run(work / "checkpoint-package", work / "checkpoint-runs")
    check(len(rm["runs"]) == 6 and rm["status"] == "complete", "imported checkpoint branches and repeats")
    for hz in ("unstimulated", "1hz", "10hz"):
        a = p.read_json(run / f"seed-42/{hz}/repeat-001/result_summary.json")
        b = p.read_json(work / f"checkpoint-runs/seed-42/{hz}/repeat-001/result_summary.json")
        check(a["final_synapse_weights"] == b["final_synapse_weights"] and a["neuron_spikes"] == b["neuron_spikes"], hz + " archive and imported-memory input agree")
    # Validation failures must happen before creating a run output or stepping.
    rejects(lambda: p.validate(bundle, run), "output conflict")
    rejects(lambda: p.validate(bundle, Path("E:/asp/26_27/labs/package-test")), "labs remains read-only")
    altered = bundle / m["branches"][0]["commands"]
    original = altered.read_bytes()
    altered.write_bytes(original + b" ")
    rejects(lambda: p.validate(bundle, work / "must-not-exist"), "tampered checksum")
    altered.write_bytes(original)
    saved = altered.with_suffix(".saved")
    altered.rename(saved)
    rejects(lambda: p.validate(bundle), "missing file")
    saved.rename(altered)
    check(not (work / "must-not-exist").exists(), "preflight failure creates no experiment")
    manifest = bundle / "manifest.json"
    original_manifest = manifest.read_bytes()
    changed = copy.deepcopy(m)
    changed["schema_version"] = 999
    p.write_json(manifest, changed)
    rejects(lambda: p.validate(bundle), "incompatible package schema")
    manifest.write_bytes(original_manifest)
    badspec = copy.deepcopy(ms)
    badspec["experiments"].append({"id": "other", "noise_seed": 43})
    p.write_json(memory / "bad-seed.json", badspec)
    rejects(lambda: p.prepare(memory / "bad-seed.json", work / "bad-seed-package", cli), "cannot mutate checkpoint noise seed")
    archived = copy.deepcopy(ms)
    archived["checkpoint"] = str(run / "seed-42/common/background.nscp")
    archived["config"] = str(memory / "archived-config.json")
    p.write_json(memory / "archived-config.json", p.probe(cli, Path(archived["checkpoint"]), True)["config"])
    p.write_json(memory / "archived-spec.json", archived)
    rejects(lambda: p.prepare(memory / "archived-spec.json", work / "archive-import-rejected", cli), "archive-linked import fails explicitly", "external archive")
    truncated = memory / "truncated.nscp"
    truncated.write_bytes((memory / "common.nscp").read_bytes()[:90])
    rejects(lambda: p.probe(cli, truncated, True), "truncated checkpoint")
    # Preserve valid integrity checksum while changing build identity: compatibility
    # must be checked by the actual NSCP loader, not bypassed by the package tool.
    blob = bytearray((memory / "common.nscp").read_bytes())
    build = m["build"]["checkpoint_build"].encode()
    at = blob.index(build)
    blob[at] = ord("0") if blob[at] != ord("0") else ord("1")
    h = 14695981039346656037
    for b in blob[:-8]:
        h = ((h ^ b) * 1099511628211) & ((1 << 64) - 1)
    blob[-8:] = struct.pack("<Q", h)
    incompatible = memory / "incompatible.nscp"
    incompatible.write_bytes(blob)
    rejects(lambda: p.probe(cli, incompatible, True), "incompatible solver checkpoint", "incompatible solver build")
    commands = p.read_json(bundle / m["branches"][1]["commands"])
    for label, modify in (("before common state", lambda d: d["commands"][0].update(time=.19)),
                          ("beyond final time", lambda d: d["commands"][0].update(time=2)),
                          ("implicit spatial recipients", lambda d: d["commands"][0]["value"].update(spatial=True))):
        bad = copy.deepcopy(commands)
        modify(bad)
        rejects(lambda: p.check_commands(bad, .2, 1.4), label)
    bad = copy.deepcopy(commands)
    bad["commands"][0]["value"]["recipients"] = [99999]
    p.write_json(work / "invalid-recipient.json", bad)
    rejects(lambda: p.probe(cli, memory / "common.nscp", True, commands=work / "invalid-recipient.json"), "native graph/protocol validation")
    profile = p.read_json(spec.parent / "profile.json")
    profile["settings"]["reference_stimulator"] = "999"
    p.write_json(work / "invalid-profile.json", profile)
    rejects(lambda: p.probe(cli, memory / "common.nscp", True, profile=work / "invalid-profile.json"), "unknown analysis reference")
    wrong = p.read_json(spec)
    wrong["analysis_interval"]["to_s"] = 400
    rejects(lambda: p.check_spec(wrong), "analysis outside planned coverage")
    a, b = work / "comparison-a.jsonl", work / "comparison-b.jsonl"
    a.write_text('["spike",1.0,0]\n', encoding="utf-8")
    b.write_text('["spike",1.000000001,0]\n', encoding="utf-8")
    cmp = p.compare_stream(a, b, 1e-8, (1,))
    check(not cmp["exact"] and cmp["within_tolerance"], "exact vs tolerance explicitly distinguished")
    b.write_text('["spike",1.0,1]\n', encoding="utf-8")
    check(not p.compare_stream(a, b, 100, (1,))["within_tolerance"], "tolerance cannot hide identity mismatch")
    b.write_text('', encoding="utf-8")
    check(not p.compare_stream(a, b, 100, (1,))["within_tolerance"], "tolerance cannot hide missing spike")
    check(not p.compare_weights([1.0], [1.0000000001], 1e-9, 0)["exact"] and p.compare_weights([1.0], [1.0000000001], 1e-9, 0)["within_tolerance"], "weight tolerance vs exact")
    rejects(lambda: p.compare_runs(run, dict(r["runs"][0], topology_sha256="different"), r["runs"][1], m["tolerances"]), "incompatible topology refuses weight pairing")
    # Moving an input bundle is safe: no absolute input paths are used at run time.
    moved = work / "relocated package"
    shutil.copytree(bundle, moved)
    check(p.validate(moved)["status"] == "valid", "relocated self-contained package validates")
    standalone = subprocess.run([sys.executable, "-X", "utf8", str(moved / "runtime/package.py"), "validate", "--package", str(moved)], cwd=work, capture_output=True, encoding="utf-8")
    check(standalone.returncode == 0 and json.loads(standalone.stdout)["status"] == "valid", "packaged runner executes without repository imports")
    journal = copy.deepcopy(commands)
    change = journal["commands"].pop()
    change["value"]["spatial"] = True
    change["recipients"] = [1, 3]
    journal["changes"] = [change]
    accepted = p.accepted_protocol(journal)["commands"][0]
    check(not accepted["value"]["spatial"] and accepted["value"]["recipients"] == [1, 3] and accepted["time"] == change["time"], "accepted GUI journal keeps exact times and resolved recipients")
    # Resource validation/coverage cannot be laundered by recomputing file hashes.
    bad = copy.deepcopy(m)
    bad["experiments"][0]["noise_seed"] = 99
    p.write_json(manifest, bad)
    rejects(lambda: p.validate(bundle), "declared seed inconsistent with Config")
    manifest.write_bytes(original_manifest)
    p.write_json(work / "test-results.json", {"checks": len(passed), "passed": passed, "wall_s": time.perf_counter() - start,
        "model_end_s": 1.4, "laboratory_400_s_executed": False})
    print(f"PASS {len(passed)} checks, {time.perf_counter()-start:.3f} wall s; {work}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    main(args.cli.resolve(), args.output_root.resolve())
