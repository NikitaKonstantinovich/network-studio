"""Derive parallel-solver fixtures and verify the frozen runner release; no simulation is run."""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DURATIONS = (.005, .05, .1, .5)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_new_or_same(path, data):
    if path.exists():
        if path.read_bytes() != data:
            raise RuntimeError(f"Refusing to overwrite changed artifact: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--durations", type=float, nargs="+", choices=DURATIONS, default=list(DURATIONS))
    args = parser.parse_args()
    previous = ROOT / "performance/20261019_single_thread"
    source = previous / "fixture.json"
    raw = source.read_bytes(); original = json.loads(raw)
    previous_provenance = json.loads((previous / "provenance.json").read_text(encoding="utf-8-sig"))
    if sha(raw) != previous_provenance["fixture_sha256"]:
        raise RuntimeError("Previous fixture differs from its recorded provenance")
    if original.get("stimulators") or any(n["current"] != 0 or n["amplitude"] != 0 or n["individual_noise"] for n in original["neurons"]):
        raise RuntimeError("Quiet derivation expects zero individual currents/pulses, no individual noise and no shared sources")
    if original["noise_sigma"] != 5 or original["duration"] != .5:
        raise RuntimeError("Unexpected source noise or duration")
    fixtures = []
    for duration in sorted(set(args.durations)):
        for workload in ("burst", "quiet"):
            data = copy.deepcopy(original)
            data["duration"] = duration
            changes = {}
            if duration != original["duration"]:
                changes["duration"] = {"old": original["duration"], "new": duration}
            if workload == "quiet":
                data["noise_sigma"] = 0
                changes["noise_sigma"] = {"old": 5, "new": 0}
            if changes:
                data["name"] = f"Performance / parallel / {workload} / {duration:g}s"
                changes["name"] = {"old": original["name"], "new": data["name"]}
            # Full burst is a byte-for-byte copy, allowing existing full references.
            encoded = raw if not changes else (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
            path = HERE / "fixtures" / f"{workload}_{duration * 1000:g}ms.json"
            write_new_or_same(path, encoded)
            fixtures.append({"path": str(path.relative_to(HERE)), "workload": workload,
                             "model_seconds": duration, "expected_macro_steps": round(duration / data["dt"]),
                             "sha256": sha(encoded), "bytes": len(encoded), "changes_from_source": changes,
                             "quiet_label": "Synthetic no-noise control; actual event count must be checked after running" if workload == "quiet" else None})
    old = ROOT / "performance/20261019_runner"
    manifest_path = old / "build_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    frozen = []
    for name in ("network_studio.exe", "network_studio_cli.exe", "network_studio_benchmark.exe", "network_studio_runner_benchmark.exe"):
        path = old / "runner-release" / name
        actual = sha(path.read_bytes()); expected = manifest["executables"][name]["sha256"]
        if actual != expected:
            raise RuntimeError(f"Frozen runner-release checksum differs: {name}")
        frozen.append({"path": "../20261019_runner/runner-release/" + name, "sha256": actual,
                       "bytes": path.stat().st_size, "matches_previous_manifest": True})
    report = {"prepared_utc": datetime.now(timezone.utc).isoformat(),
              "source": "../20261019_single_thread/fixture.json", "source_sha256": sha(raw),
              "origin": previous_provenance["origin_note"],
              "unchanged": {"neurons": len(original["neurons"]), "synapses": len(original["synapses"]),
                            "seed": original["seed"], "dt": original["dt"], "sample": original["sample"],
                            "noise_interval": original["noise_interval"], "recording": "Same selected neurons/synapses", "stages": "Preserved but no stage is activated by the benchmark"},
              "fixtures": fixtures, "frozen_release": frozen,
              "frozen_build_manifest": "../20261019_runner/build_manifest.json",
              "frozen_build_manifest_sha256": sha(manifest_path.read_bytes()),
              "frozen_checkpoint_build_header": manifest["checkpoint_build_header"],
              "frozen_copy_policy": "Reuse existing immutable runner-release in place; never rebuild or overwrite it",
              "status": "Prepared configurations only; no solver, benchmark, test, or build was run"}
    path = HERE / "provenance.json"
    # The generated inventory may be refreshed to add durations; fixture bytes cannot be overwritten.
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(fixtures)} fixtures; verified {len(frozen)} frozen executables")


if __name__ == "__main__":
    main()
