"""Focused 10 Hz comparison: translated NeuroNet Euler versus the actual studio CLI.

This script NEVER executes NeuroNet. Its Euler calculation translates selected
instructions from the audited SM R05 binary; it is not an original event log.
Only Python's standard library and a built network_studio_cli are required.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys


LAB = Path(__file__).resolve().parent
DURATION_MS = 3000.0
STEADY_BEGIN_MS = 1000.0


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def event_statistics(events_ms: list[float]) -> dict:
    steady = [t for t in events_ms if STEADY_BEGIN_MS <= t < DURATION_MS]
    return {
        "total_events": len(events_ms),
        "steady_window_ms": [STEADY_BEGIN_MS, DURATION_MS],
        "steady_events": len(steady),
        "steady_rate_hz": len(steady) * 1000 / (DURATION_MS - STEADY_BEGIN_MS),
        "events_per_100ms_period": [
            sum(100 * i <= t < 100 * (i + 1) for t in events_ms)
            for i in range(int(DURATION_MS / 100))
        ],
        "last_period_events_ms": [t for t in events_ms if t >= DURATION_MS - 100],
    }


def translated_euler(dt_ms: float) -> dict:
    # Explicitly chosen initial state, not reconstructed from the GUI session.
    # The audited constructor uses V=-70, u=0. The comparison uses a late train.
    voltage, recovery, time = -70.0, 0.0, 0.0
    spike_flag, timer_ms = False, 0.0
    thresholds, registered = [], []
    for _ in range(round(DURATION_MS / dt_ms)):
        # Original normal rhythmic stimulator: quantized time, inclusive row end.
        phase = (int(time * 10) % (int(100) * 10)) / 10
        current = 30.0 if 50 < phase <= 53 else 0.0
        dv = .04 * voltage * voltage + 5 * voltage + 140 - recovery + current
        du = .02 * (.2 * voltage - recovery)
        voltage += dt_ms * dv
        recovery += dt_ms * du
        if voltage >= 30:
            thresholds.append(time)
            spike_flag = True
            voltage = -65.0
            recovery += 8.0

        # Spike showing=1 is an output/event timer, not a pause of V/u above.
        if timer_ms != 0:
            timer_ms -= dt_ms
            if timer_ms <= 0:
                spike_flag, timer_ms = False, 0.0
        if timer_ms == 0 and spike_flag:
            registered.append(time)
            timer_ms = 1.0
        time += dt_ms

    if thresholds != registered:
        raise RuntimeError("The selected experiment unexpectedly gates a threshold event.")
    return {
        "evidence_kind": "Python translation; not execution of NeuroNet",
        "dt_ms": dt_ms,
        "initial_V_mV": -70.0,
        "initial_u": 0.0,
        "timestamp_convention": "step start, before incrementing the cumulative clock",
        "threshold_times_ms": thresholds,
        "registered_times_ms": registered,
        "spike_showing_ms": 1.0,
        "frequency_window_ms": 50.0,
        "display_examples": [
            {"time_ms": t, "Frequency": 1000 / 50 * sum(t - 50 <= e <= t for e in registered)}
            for t in (2925.0, 2975.0)
        ],
        **event_statistics(registered),
    }


def run_cli(cli: Path, destination: Path) -> dict:
    config_path = destination / "rk4_10hz_30x3.json"
    # Obtain all schema-required shared parameters from this build's preset.
    subprocess.run([str(cli), "--preset", "4", "--save", str(config_path)],
                   check=True, capture_output=True, text=True)
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    config.update(name="NeuroNet audit: RS, 10 Hz, 30 x 3 ms", duration=3.0,
                  dt=.000025, sample=.0005, noise_sigma=0.0,
                  noise_interval=.001, seed=42, astrocytes=[], synapses=[], gaps=[])
    neuron = config["neurons"][0]
    neuron.update(name="RS", model="izhikevich", voltage=-70.0, current=0.0,
                  amplitude=30.0, frequency=10.0, start=.05, stop=3.0, width=.003,
                  izh_a=.02, izh_b=.2, izh_c=-65.0, izh_d=8.0, izh_peak=30.0,
                  izh_u=0.0, equilibrium_u=False, individual_noise=False, noise_sigma=0.0)
    config["neurons"] = [neuron]
    write_json(config_path, config)
    prefix = destination / "rk4_10hz_30x3"
    completed = subprocess.run([str(cli), "--config", str(config_path), "--output", str(prefix)],
                               check=True, capture_output=True, text=True)
    (destination / "rk4_10hz_30x3_cli.log").write_text(completed.stdout + completed.stderr,
                                                     encoding="utf-8")
    with prefix.with_name(prefix.name + "_events.csv").open(encoding="utf-8", newline="") as stream:
        events = [1000 * float(row["time_s"]) for row in csv.DictReader(stream)
                  if row["type"] == "spike" and row["node_index_1based"] == "1"]
    native_summary = json.loads(prefix.with_name(prefix.name + "_summary.json").read_text(encoding="utf-8"))
    if abs(native_summary["time_s"] - 3.0) > 1e-9:
        raise RuntimeError("The studio CLI did not reach the requested duration.")
    return {
        "evidence_kind": "execution of the actual network_studio_cli",
        "executable_filename": cli.name,
        "executable_sha256": digest(cli),
        "dt_ms": .025,
        "method": "RK4, voltage threshold/reset at integration step end",
        "timestamp_convention": "step end, exported by the studio CLI",
        "stimulus": "unquantized rectangular 30-unit pulses, [50,53) ms modulo 100 ms",
        "configuration": config_path.name,
        "configuration_sha256": digest(config_path),
        "event_log": prefix.name + "_events.csv",
        **event_statistics(events),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=LAB / "results" / "neuronet_comparison")
    parser.add_argument("--cli", type=Path,
                        default=LAB.parent / "build" / "Release" / "network_studio_cli.exe")
    args = parser.parse_args()
    cli, destination = args.cli.resolve(), args.output.resolve()
    if not cli.is_file():
        parser.error("Build network_studio_cli first, or provide its path with --cli.")
    destination.mkdir(parents=True, exist_ok=True)
    translated = [translated_euler(dt) for dt in (.5, .025)]
    native = run_cli(cli, destination)
    results = {
        "scope": "One RS neuron, noise zero, 10 Hz stimulation, amplitude 30, duration 3 ms",
        "python_version": sys.version.split()[0],
        "script_sha256": digest(Path(__file__)),
        "simulation_duration_ms": DURATION_MS,
        "translation_limitations": [
            "Python binary64 intermediates can differ from original x87 extended precision.",
            "Original GUI starting state and prior history were not exported.",
            "Translated timestamps and CLI timestamps use different step conventions.",
            "Original stimulator quantizes phase to 0.1 ms; studio pulses are unquantized.",
            "This is a steady train comparison, not a rested isolated-pulse experiment.",
        ],
        "translated_neuronet": translated,
        "studio_cli": native,
    }
    write_json(destination / "probe_results.json", results)
    lines = [
        "Original NeuroNet observations are documented separately in README.md; no raw event log was exported.",
        "Steady window: [1000, 3000) ms; stimulus: 10 Hz, amplitude 30, width 3 ms.",
    ]
    for result in translated:
        lines.append(f"Python translation, Euler {result['dt_ms']:g} ms: "
                     f"{result['steady_events']} events, {result['steady_rate_hz']:g} Hz.")
    lines.append(f"Actual studio CLI, RK4 0.025 ms: {native['steady_events']} events, "
                 f"{native['steady_rate_hz']:g} Hz.")
    report = "\n".join(lines) + "\n"
    (destination / "probe_stdout.txt").write_text(report, encoding="utf-8")
    print(report, end="")


if __name__ == "__main__":
    main()
