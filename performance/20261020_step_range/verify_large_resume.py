"""New-process checkpoint continuation against a completed measure.py run."""
import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--measured", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    measured = json.loads((args.measured / "report.json").read_text(encoding="utf-8"))
    config = args.measured / "dt_500.json"
    reference = args.measured / "dt_500_repeat_1"
    assert sha(config) == measured["copies"]["500"]["sha256"]
    cli = args.cli.resolve()
    pause, final = args.output / "pause.nscp", args.output / "resumed.nscp"
    commands = []
    started = time.perf_counter()
    for name, options in (
        ("pause", ["--config", config, "--stop-at", ".05", "--checkpoint", pause]),
        ("resumed", ["--resume", pause, "--stop-at", ".1", "--checkpoint", final]),
    ):
        command = [str(cli), *map(str, options), "--threads", str(measured["threads_requested"]),
                   "--output", str(args.output / name)]
        completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
        (args.output / (name + ".log")).write_text(completed.stdout + completed.stderr, encoding="utf-8")
        assert completed.returncode == 0, completed.stdout + completed.stderr
        commands.append(command)
    assert final.read_bytes() == (reference / "final.nscp").read_bytes()
    compared = []
    for file in reference.glob("result_*.csv"):
        assert file.read_bytes() == (args.output / file.name.replace("result_", "resumed_", 1)).read_bytes()
        compared.append(file.name)
    assert json.loads((args.output / "resumed_summary.json").read_text()) == json.loads(
        (reference / "result_summary.json").read_text())
    assert sha(config) == measured["copies"]["500"]["sha256"]
    report = {"commands": commands, "elapsed_seconds": time.perf_counter() - started,
              "model_time_s": .1, "split_time_s": .05, "dt_us": 500,
              "checkpoint_byte_exact": True, "summary_exact": True, "csv_pairs": compared,
              "cli_sha256": sha(cli), "final_checkpoint_sha256": sha(final)}
    (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"PASS large-network new-process resume; {len(compared)} exact CSV pairs; {report['elapsed_seconds']:.3f} s")


if __name__ == "__main__":
    main()
