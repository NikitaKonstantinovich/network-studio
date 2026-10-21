"""CLI/JSON/new-process resume checks; existing configurations are read-only."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=args.work))
    cli = args.cli.resolve()
    commands = []

    def run(options, name, error=None, executable=cli):
        command = [str(executable), *map(str, options), "--output", str(out / name)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
        (out / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        commands.append(command)
        if error is None:
            assert result.returncode == 0, result.stdout + result.stderr
        else:
            assert result.returncode != 0 and error in result.stderr, result.stdout + result.stderr
        return result

    base = out / "base.json"
    run(["--preset", 4, "--duration", ".02", "--sample", ".001", "--save", base], "initial")
    config = json.loads(base.read_text(encoding="utf-8"))
    config["dt"] = .0005
    config["noise_sigma"] = .3
    config["neurons"][0]["current"] = 10
    path = out / "valid.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    original = path.read_bytes()
    run(["--config", path, "--stop-at", ".005", "--checkpoint", out / "pause.nscp"], "part")
    run(["--resume", out / "pause.nscp", "--dt", ".00025"], "resume_dt_rejected", "configuration overrides")
    run(["--resume", out / "pause.nscp", "--threads", 6, "--checkpoint", out / "resumed.nscp"], "resumed")
    run(["--config", path, "--checkpoint", out / "continuous.nscp"], "continuous")
    assert (out / "resumed.nscp").read_bytes() == (out / "continuous.nscp").read_bytes()
    compared = []
    for file in out.glob("continuous_*.csv"):
        assert file.read_bytes() == (out / file.name.replace("continuous_", "resumed_", 1)).read_bytes()
        compared.append(file.name)
    assert path.read_bytes() == original
    summary = json.loads((out / "continuous_summary.json").read_text())
    stats = summary["integration_statistics"]
    assert isinstance(stats["accepted_internal_steps"], str) and int(stats["accepted_internal_steps"]) >= summary["steps"]
    assert stats["mean_internal_step_s"] == summary["time_s"] / int(stats["accepted_internal_steps"])
    for dt in (1e-6, 25e-6, 50e-6, 100e-6, 250e-6, 500e-6):
        run(["--config", path, "--dt", dt, "--run-for", ".001"], f"range_{dt}")
    for option, value, error in (("--dt", .000500001, "1..500"),
                                 ("--dt", .000000999, "1..500"),
                                 ("--sample", .0001, "sample")):
        run(["--config", path, option, value, "--run-for", 0], option[2:] + str(value), error)
    for kind in ("hh", "mixed", "noise"):
        bad = json.loads(json.dumps(config))
        if kind == "noise":
            bad["noise_interval"] = .0001
        elif kind == "hh":
            bad["neurons"][0]["model"] = "hh"
        else:
            bad["neurons"].append(dict(bad["neurons"][0], model="hh", name="HH"))
        broken = out / (kind + ".json")
        broken.write_text(json.dumps(bad), encoding="utf-8")
        # Use the actual legacy spelling emitted by the serializer.
        if kind != "noise":
            hh = out / "hh_source.json"
            run(["--preset", 3, "--duration", ".002", "--save", hh], "hh_source_" + kind)
            hh_model = json.loads(hh.read_text())["neurons"][0]["model"]
            bad["neurons"][-1 if kind == "mixed" else 0]["model"] = hh_model
            broken.write_text(json.dumps(bad), encoding="utf-8")
        run(["--config", broken, "--run-for", 0], "invalid_" + kind, "noise_interval" if kind == "noise" else "1..50")
    # The incompatible baseline must reject a new checkpoint through its ordinary loader.
    baseline_pairs = 0
    if args.baseline:
        old = args.baseline.resolve()
        run(["--resume", out / "continuous.nscp"], "old_reject", "unsupported schema", old)
        for preset in range(8):
            options = ["--preset", preset, "--duration", ".12", "--sample", ".001"]
            run(options, f"old_{preset}", executable=old)
            run(options, f"new_{preset}")
            for suffix in ("trace", "events", "deliveries", "pending_deliveries", "weights", "stimuli", "shared_stimuli"):
                assert (out / f"old_{preset}_{suffix}.csv").read_bytes() == (out / f"new_{preset}_{suffix}.csv").read_bytes()
                baseline_pairs += 1
    report = {"commands": commands, "checkpoint_byte_exact": True, "csv_pairs": compared,
              "old_new_csv_pairs": baseline_pairs, "cli_sha256": hashlib.sha256(cli.read_bytes()).hexdigest()}
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"PASS CLI/JSON/resume; {len(compared)} exact CSV pairs; {baseline_pairs} baseline comparisons; {out}")


if __name__ == "__main__":
    main()
