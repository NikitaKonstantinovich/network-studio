"""Compare the production CLI with an independent adaptive, event-located ODE.

Requires scipy. All reference integration is in milliseconds. The reference
splits at stimulus discontinuities and locates v=30 before applying the reset.
Run: python reference_check.py --cli build/Release/network_studio_cli.exe
"""
import argparse
import json
import subprocess
from pathlib import Path

from scipy.integrate import solve_ivp


def reference(config):
    n = config["neurons"][0]
    end = config["duration"] * 1000
    edges = {0., end}
    pulses = []
    if n["amplitude"] and n["frequency"] > 0:
        start = n["start"] * 1000
        while start < min(end, n["stop"] * 1000) - 1e-8:
            stop = min(start + n["width"] * 1000, end, n["stop"] * 1000)
            pulses.append((start, stop))
            edges.update((start, stop))
            start += 1000 / n["frequency"]
    state = [n["voltage"], n["izh_b"] * n["voltage"] if n["equilibrium_u"] else n["izh_u"]]
    events = []

    def peak(t, y):
        return y[0] - n["izh_peak"]
    peak.terminal = True
    peak.direction = 1
    boundaries = sorted(edges)
    for left, right in zip(boundaries[:-1], boundaries[1:]):
        middle = (left + right) / 2
        current = n["current"] + (n["amplitude"] if any(a <= middle < b for a, b in pulses) else 0)

        def rhs(t, y):
            v, u = y
            return [.04*v*v + 5*v + 140 - u + current, n["izh_a"] * (n["izh_b"]*v - u)]

        time = left
        while time < right - 1e-10:
            solution = solve_ivp(rhs, (time, right), state, events=peak,
                                 rtol=1e-10, atol=1e-12, max_step=.2)
            if not solution.success:
                raise RuntimeError(solution.message)
            state = solution.y[:, -1].tolist()
            time = solution.t[-1]
            if len(solution.t_events[0]):
                events.append(float(time / 1000))
                state = [n["izh_c"], state[1] + n["izh_d"]]
    return events


def main():
    import csv
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent
    parser.add_argument("--cli", type=Path, default=root / "build/Release/network_studio_cli.exe")
    parser.add_argument("--output", type=Path, default=root / "verification/reference")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base_file = args.output / "base.json"
    subprocess.run([str(args.cli), "--preset", "4", "--save", str(base_file)], check=True, capture_output=True)
    base = json.loads(base_file.read_text(encoding="utf-8"))
    cases = [("recommended_30x3", 2., .003, .2, 0.),
             ("pulse_2hz", 2., .001, .2, 0.),
             ("pulse_10hz", 10., .001, .2, 0.),
             ("pulse_20hz", 20., .001, .2, 0.),
             ("autonomous_b027", 0., 0., .27, 0.),
             ("dc_4", 0., 0., .2, 4.)]
    report = {"reference": "SciPy solve_ivp RK45; exact pulse boundaries; terminal v=peak event; rtol=1e-10, atol=1e-12, max_step=.2ms", "cases": []}
    for name, rate, width, b, dc in cases:
        config = json.loads(json.dumps(base))
        config.update(duration=2.1, sample=.0005)
        n = config["neurons"][0]
        n.update(frequency=rate, width=width, izh_b=b, current=dc, stop=2.1)
        expected = reference(config)
        errors = []
        for dt in (.000025, .0000125):
            config["dt"] = dt
            prefix = args.output / (name + ("_fine" if dt < .00002 else "_coarse"))
            path = prefix.with_suffix(".json")
            path.write_text(json.dumps(config), encoding="utf-8")
            subprocess.run([str(args.cli), "--config", str(path), "--output", str(prefix)], check=True, capture_output=True)
            with open(str(prefix) + "_events.csv", newline="", encoding="utf-8") as stream:
                actual = [float(row["time_s"]) for row in csv.DictReader(stream) if row["type"] == "spike"]
            if len(actual) != len(expected):
                raise AssertionError(f"{name}: count mismatch CLI={len(actual)}, reference={len(expected)}")
            error_ms = max((abs(a-b)*1000 for a, b in zip(actual, expected)), default=0)
            # Fixed-grid resets accumulate phase error in autonomous firing.
            # This bound checks useful agreement over 2.1s, not fourth-order hybrid accuracy.
            if error_ms > 5:
                raise AssertionError(f"{name}: spike time discrepancy {error_ms:.6g} ms")
            errors.append(error_ms)
        if errors[1] > errors[0] + .03:
            raise AssertionError(f"{name}: refining dt did not improve reference agreement")
        report["cases"].append(dict(name=name, spikes=len(expected), max_error_ms_dt25us=errors[0], max_error_ms_dt12_5us=errors[1]))
        print(name, len(expected), "spikes; max errors ms:", errors, flush=True)
    (args.output / "reference_summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
