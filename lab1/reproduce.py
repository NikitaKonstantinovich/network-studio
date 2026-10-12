"""Reproduce laboratory 1 using the exact C++ core used by Network Studio.

No neuron or synapse solver is implemented in this script: all traces come from
network_studio_cli. Python only prepares configurations, checks and plots results.
"""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
from config_compat import canonical_config

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parent
CONFIGS = HERE / "configs"
RESULTS = HERE / "results"
RAW = RESULTS / "raw"
FIGURES = RESULTS / "figures"
SOURCE_SHA256 = "ce57a4758a90f3a2a9ff9c57f001d5e40363bf02ddb1630a3a2d1ec9efff8af3"


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def neuron(name="RS", **changes):
    value = dict(name=name, px=.3, py=.5, voltage=-65., current=0., amplitude=0.,
                 frequency=10., start=.1, stop=3.1, width=.001,
                 model="izhikevich", izh_a=.02, izh_b=.2, izh_c=-65.,
                 izh_d=8., izh_peak=30., izh_u=-13., equilibrium_u=True,
                 individual_noise=False, noise_sigma=0.)
    value.update(changes)
    return value


def configuration(name, neurons=None, duration=3.2, **changes):
    value = copy.deepcopy(json.loads((STUDIO / "examples" / "two_inputs.json").read_text(encoding="utf-8")))
    value.update(schema=2, name=name, dt=.000025, duration=duration, sample=.0001,
                 noise_sigma=0., noise_interval=.001, seed=42,
                 neurons=neurons or [neuron()], astrocytes=[], synapses=[], gaps=[])
    value.update(changes)
    return value


def prepare():
    scenarios = {}
    scenarios["01_rest"] = configuration("Lab 1 / task 1 / RS at rest", duration=.4)
    single = configuration("Lab 1 / task 1 / single pulse and TM output",
                           [neuron("Stimulated RS", amplitude=30., stop=.101),
                            neuron("Passive receiver", px=.7)], duration=.4, sample=.00005)
    single["synapses"] = [dict(source=0, target=1, astro=-1,
                                model="tsodyks_markram", weight=.01, gain=20.,
                                conductance=.25, reversal=0., release=.5,
                                tau_syn=.01, tau_f=1., tau_d=.05,
                                spill=.01, enabled=True)]
    scenarios["02_single_pulse"] = single
    reference = copy.deepcopy(single)
    reference["name"] = "Lab 1 / textbook recommendation / 30 by 3 ms"
    reference["neurons"][0].update(width=.003, stop=.103)
    scenarios["02_reference_30x3ms"] = reference
    scenarios["03_autonomous"] = configuration("Lab 1 / task 2 / autonomous b=0.27",
                                               [neuron("Autonomous RS variant", izh_b=.27)], duration=5.)
    for hz in [2, 10, 20]:
        scenarios[f"04_periodic_{hz:02}hz"] = configuration(
            f"Lab 1 / task 3 / {hz} Hz", [neuron(f"RS / {hz} Hz", amplitude=30., frequency=float(hz))])
    reference_train = copy.deepcopy(scenarios["04_periodic_10hz"])
    reference_train["name"] = "Lab 1 / textbook recommendation / 10 Hz, 30 by 3 ms"
    reference_train["neurons"][0]["width"] = .003
    scenarios["04_reference_10hz_30x3ms"] = reference_train
    scenarios["05_dc_4"] = configuration("Lab 1 / task 3 / constant current 4",
                                        [neuron("RS / I=4", current=4.)], duration=5.)
    for sigma in [0, 5, 8]:
        scenarios[f"06_noise_global_{sigma}"] = configuration(
            f"Lab 1 / task 4 / global noise sigma={sigma}", duration=5., noise_sigma=float(sigma))
    scenarios["07_noise_individual"] = configuration(
        "Lab 1 / task 4 / global 5 and individual 8",
        [neuron("Global sigma=5", px=.3),
         neuron("Individual sigma=8", px=.7, individual_noise=True, noise_sigma=8.)],
        duration=5., noise_sigma=5.)
    main_names = list(scenarios)
    for current in [0., 2., 3., 3.5, 3.7, 3.75, 3.8, 3.9, 4., 4.2, 4.5, 5., 6., 8.]:
        key = f"sweep_dc_{current:.2f}".replace(".", "p")
        scenarios[key] = configuration(f"Lab 1 / DC sweep I={current:g}",
                                       [neuron(current=current)], duration=5., sample=.0002)
    for sigma in [5, 8]:
        for seed in [11, 23, 42, 71, 101]:
            scenarios[f"noise_sigma{sigma}_seed{seed}"] = configuration(
                f"Lab 1 / noise replicates / sigma={sigma}, seed={seed}",
                duration=10., sample=.0002, noise_sigma=float(sigma), seed=seed)
    deterministic = [name for name in main_names if "reference" not in name and name.startswith(("01_", "02_", "03_", "04_", "05_"))]
    for name in deterministic:
        fine = copy.deepcopy(scenarios[name])
        fine["name"] += " / dt=12.5 us"
        fine["dt"] /= 2.
        scenarios[f"fine_{name}"] = fine
    scenarios["repeat_noise"] = copy.deepcopy(scenarios["07_noise_individual"])
    for name, config in scenarios.items():
        write_json(CONFIGS / f"{name}.json", config)
    write_json(RESULTS / "manifest.json", dict(schema=1, main=main_names,
               scenarios=list(scenarios), deterministic=deterministic,
               source="UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf",
               source_sha256=SOURCE_SHA256,
               source_pages=dict(model=[5, 6, 7], laboratory=[12, 13, 14, 15, 16, 17]),
               solver="Network Studio C++ core / network_studio_cli",
               noise="Independent Gaussian current, sample-and-hold 1 ms; not a continuous-time SDE"))
    return scenarios, main_names, deterministic


def execute(cli, scenarios):
    started = time.perf_counter()
    RAW.mkdir(parents=True, exist_ok=True)
    if not cli.is_file():
        raise RuntimeError(f"CLI executable not found: {cli}. Build Network Studio first.")
    run_provenance = dict(cli_sha256=digest(cli),
        source_files={name: digest(STUDIO / name) for name in ["network.h", "network.cpp", "io.cpp"]},
        config_sha256={name: digest(CONFIGS / f"{name}.json") for name in scenarios})
    for index, name in enumerate(scenarios, 1):
        print(f"[{index}/{len(scenarios)}] {name}", flush=True)
        completed = subprocess.run([str(cli), "--config", str(CONFIGS / f"{name}.json"),
                                    "--output", str(RAW / name)],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace")
        (RAW / f"{name}_cli.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(f"CLI failed for {name}: {completed.stderr.strip()}")
    if digest(cli) != run_provenance["cli_sha256"]:
        raise RuntimeError("CLI changed during reproduction; repeat with one stable executable.")
    run_provenance.update(source_pdf_sha256=SOURCE_SHA256,
                          cli_runs_elapsed_seconds=round(time.perf_counter() - started, 3))
    write_json(RESULTS / "run_provenance.json", run_provenance)


def imports(python_deps=None):
    # Explicit dependency directories must match the selected Python ABI.
    os.environ.setdefault("MPLCONFIGDIR", str(RAW / "matplotlib_cache"))
    if python_deps is not None:
        sys.path.insert(0, str(python_deps.resolve()))
    global np, plt
    import numpy as np
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.grid": True, "grid.alpha": .2,
                         "figure.dpi": 120, "savefig.dpi": 180,
                         "axes.titleweight": "bold"})


def events(name, neuron_index=1):
    with (RAW / f"{name}_events.csv").open(encoding="utf-8-sig", newline="") as file:
        return np.array([float(row["time_s"]) for row in csv.DictReader(file)
                         if row["type"] == "spike" and int(row["node_index_1based"]) == neuron_index])


def trace(name):
    data = np.genfromtxt(RAW / f"{name}_trace.csv", delimiter=",", names=True, encoding="utf-8-sig")
    return data


def stats(times, duration, discard=0.):
    selected = times[(times >= discard) & (times < duration)]
    isi = np.diff(selected)
    return dict(spikes=int(len(times)), analyzed_spikes=int(len(selected)),
                analyzed_from_s=discard, analyzed_to_s=duration,
                rate_hz=float(len(selected) / (duration - discard)),
                mean_isi_ms=float(isi.mean() * 1000.) if len(isi) else None,
                isi_frequency_hz=float(1. / isi.mean()) if len(isi) else None,
                isi_cv=float(isi.std() / isi.mean()) if len(isi) else None,
                first_spike_s=float(times[0]) if len(times) else None,
                last_spike_s=float(times[-1]) if len(times) else None)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analyze(scenarios, main_names, deterministic):
    metrics = dict(scenarios={}, checks={}, convergence={}, dc_sweep=[], noise_replicates={})
    for name, config in scenarios.items():
        snapshot = json.loads((RAW / f"{name}_experiment.json").read_text(encoding="utf-8-sig"))
        def check_snapshot(expected, actual, context):
            if isinstance(expected, dict):
                for key, value in expected.items():
                    if key not in actual:
                        raise AssertionError(f"Snapshot field missing: {context}.{key}")
                    check_snapshot(value, actual[key], f"{context}.{key}")
            elif isinstance(expected, list):
                if len(expected) != len(actual):
                    raise AssertionError(f"Snapshot size mismatch: {context}")
                for index, (left, right) in enumerate(zip(expected, actual)):
                    check_snapshot(left, right, f"{context}[{index}]")
            elif expected != actual:
                raise AssertionError(f"Snapshot differs from requested configuration: {context}")
        check_snapshot(canonical_config(config), canonical_config(snapshot), name)
        summary = json.loads((RAW / f"{name}_summary.json").read_text(encoding="utf-8-sig"))
        if not summary["complete"] or summary["error"]:
            raise AssertionError(f"Incomplete simulation: {name}")
        data = trace(name)
        time = data["time_s"]
        if not all(np.isfinite(data[field]).all() for field in data.dtype.names):
            raise AssertionError(f"Nonfinite state: {name}")
        if not np.all(np.diff(time) > 0.) or abs(time[-1] - config["duration"]) > 1e-9:
            raise AssertionError(f"Invalid sampling clock: {name}")
        discard = 1. if config["duration"] >= 5. else 0.
        measures = [stats(events(name, index + 1), config["duration"], discard)
                    for index in range(len(config["neurons"]))]
        if summary["neuron_spikes"] != [m["spikes"] for m in measures]:
            raise AssertionError(f"Event count mismatch: {name}")
        metrics["scenarios"][name] = dict(neurons=measures, samples=len(time),
                                         dt_s=config["dt"], duration_s=config["duration"])
        if name in main_names:
            write_json(RESULTS / f"{name}_summary.json", summary)
        if name.startswith("sweep_dc_"):
            metrics["dc_sweep"].append(dict(current=config["neurons"][0]["current"], **measures[0]))
    assert metrics["scenarios"]["01_rest"]["neurons"][0]["spikes"] == 0, "RS rest must not spike"
    assert metrics["scenarios"]["02_single_pulse"]["neurons"][0]["spikes"] == 1, "Single pulse must yield one spike"
    metrics["checks"]["quiet_RS_and_single_spike"] = True
    metrics["checks"]["periodic"] = {}
    for hz in [2, 10, 20]:
        name = f"04_periodic_{hz:02}hz"
        times = events(name)
        pulses = .1 + np.arange(hz * 3) / hz
        counts = [int(((times >= p) & (times < p + 1. / hz)).sum()) for p in pulses]
        assert counts == [1] * len(pulses), f"Not one spike per stimulus at {hz} Hz"
        frequency = 1. / np.diff(times).mean()
        assert abs(frequency / hz - 1.) < .01, f"Unexpected output frequency at {hz} Hz"
        metrics["checks"]["periodic"][str(hz)] = dict(pulses=len(pulses), spikes=len(times),
                                                        isi_frequency_hz=float(frequency), one_spike_per_pulse=True)
    for name in ["03_autonomous", "05_dc_4"]:
        assert metrics["scenarios"][name]["neurons"][0]["analyzed_spikes"] >= 3, f"No sustained spiking: {name}"
    metrics["checks"]["autonomous_and_DC_sustained"] = True
    metrics["checks"]["noise_replay_identical"] = all(
        digest(RAW / f"07_noise_individual_{suffix}") == digest(RAW / f"repeat_noise_{suffix}")
        for suffix in ["trace.csv", "events.csv"])
    assert metrics["checks"]["noise_replay_identical"], "Noise replay is not deterministic"
    metrics["checks"]["global_noise_matches_inherited_node"] = bool(np.array_equal(
        trace("06_noise_global_5")["N1_V_mV"], trace("07_noise_individual")["N1_V_mV"]))
    assert metrics["checks"]["global_noise_matches_inherited_node"], "Global noise inheritance mismatch"
    for name in deterministic:
        base = events(name)
        fine = events(f"fine_{name}")
        matched = len(base) == len(fine)
        error = float(np.max(np.abs(base - fine)) * 1000.) if matched and len(base) else 0. if matched else None
        metrics["convergence"][name] = dict(same_spike_count=matched, coarse_spikes=len(base),
                                             fine_spikes=len(fine), max_event_time_difference_ms=error)
        assert matched, f"Spike count changed at half dt: {name}"
        assert error <= 2., f"Event timing convergence exceeds 2 ms: {name} / {error} ms"
    for sigma in [5, 8]:
        values = [metrics["scenarios"][f"noise_sigma{sigma}_seed{seed}"]["neurons"][0]
                  for seed in [11, 23, 42, 71, 101]]
        rates = [value["rate_hz"] for value in values]
        cvs = [value["isi_cv"] for value in values if value["isi_cv"] is not None]
        metrics["noise_replicates"][str(sigma)] = dict(seeds=[11, 23, 42, 71, 101],
              analysis_window_s=[1., 10.], rates_hz=rates,
              mean_rate_hz=float(np.mean(rates)), sd_rate_hz=float(np.std(rates, ddof=1)),
              mean_isi_cv=float(np.mean(cvs)) if cvs else None)
    assert metrics["noise_replicates"]["8"]["mean_rate_hz"] > metrics["noise_replicates"]["5"]["mean_rate_hz"], "Noise rate trend not reproduced"
    metrics["checks"]["noise_rate_increases_across_seeds"] = True
    write_json(RESULTS / "metrics.json", metrics)
    with (RESULTS / "dc_sweep.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=list(metrics["dc_sweep"][0]))
        writer.writeheader()
        writer.writerows(metrics["dc_sweep"])
    return metrics


def voltage(axis, name, index=1, window=None, color="#2563a6", title=None):
    data = trace(name)
    time = data["time_s"]
    if window is None:
        window = (0., float(time[-1]))
    selected = (time >= window[0]) & (time <= window[1])
    axis.plot(time[selected] * 1000., data[f"N{index}_V_mV"][selected], color=color, lw=.8)
    spike = events(name, index)
    spike = spike[(spike >= window[0]) & (spike <= window[1])]
    axis.scatter(spike * 1000., np.full(len(spike), 30.), marker="v", s=13,
                 color=color, zorder=3, label="события v ≥ 30 мВ")
    axis.set(ylabel="v, мВ", ylim=(-92, 38), xlim=np.array(window) * 1000.)
    if title:
        axis.set_title(title, loc="left", fontsize=11)
    return data


def save_figure(figure, name):
    FIGURES.mkdir(parents=True, exist_ok=True)
    figure.savefig(FIGURES / f"{name}.png", bbox_inches="tight", facecolor="white")
    plt.close(figure)


def plots(metrics):
    figure, axes = plt.subplots(4, 1, figsize=(10, 9), constrained_layout=True, sharex=True)
    voltage(axes[0], "01_rest", title="RS без входа: устойчивый покой")
    data = voltage(axes[1], "02_single_pulse", title="Подобранный одиночный стимул: I=30, 100–101 мс")
    axes[1].axvspan(100, 101, color="#e5ad39", alpha=.3)
    voltage(axes[2], "02_reference_30x3ms", color="#8e5b9a", title="Проверка рекомендации методички: I=30, 100–103 мс")
    axes[2].axvspan(100, 103, color="#e5ad39", alpha=.3)
    y_name = next(field for field in ["S1_y", "S1_s"] if field in data.dtype.names)
    axes[3].plot(data["time_s"] * 1000, 20. * data[y_name], color="#bd4f35", lw=1.4)
    axes[3].set(xlabel="Время, мс", ylabel="g·y, ед. тока", title="Выход при одиночном спайке: g=20, U=0.5, τI=10 мс")
    save_figure(figure, "01_rest_single_output")
    figure, axes = plt.subplots(2, 1, figsize=(10, 5.8), constrained_layout=True, sharex=True)
    data = voltage(axes[0], "03_autonomous", title="Автоколебания при I=0: a=0.02, b=0.27, c=−65, d=8")
    axes[1].plot(data["time_s"] * 1000, data["N1_u"], color="#bd4f35", lw=1.)
    axes[1].set(xlabel="Время, мс", ylabel="u, ед. модели")
    save_figure(figure, "02_autonomous")
    figure, axes = plt.subplots(3, 1, figsize=(10, 7.2), constrained_layout=True, sharex=True)
    for axis, hz in zip(axes, [2, 10, 20]):
        measured = metrics["checks"]["periodic"][str(hz)]["isi_frequency_hz"]
        voltage(axis, f"04_periodic_{hz:02}hz", title=f"Стимуляция {hz} Гц → частота по ISI {measured:.3f} Гц")
        for p in .1 + np.arange(hz * 3) / hz:
            axis.axvspan(p * 1000, p * 1000 + 1, color="#e5ad39", alpha=.25, lw=0)
    axes[-1].set_xlabel("Время, мс")
    save_figure(figure, "03_periodic_2_10_20hz")
    figure, axes = plt.subplots(2, 1, figsize=(10, 6.3), constrained_layout=True)
    sweep = metrics["dc_sweep"]
    axes[0].plot([r["current"] for r in sweep], [r["rate_hz"] for r in sweep], "o-", color="#2563a6", ms=4)
    axes[0].axvline(4., color="#bd4f35", lw=1, ls="--", label="I=4: выбранный режим")
    axes[0].set(xlabel="Постоянный ток I, ед. модели", ylabel="Частота, Гц",
                title="Частота по числу событий в окне 1–5 с")
    axes[0].legend(frameon=False)
    voltage(axes[1], "05_dc_4", window=(3., 5.), title="Ритмическая генерация при постоянном I=4")
    axes[1].set_xlabel("Время, мс")
    save_figure(figure, "04_dc_sweep")
    figure, axes = plt.subplots(3, 1, figsize=(10, 7.2), constrained_layout=True, sharex=True)
    for axis, sigma in zip(axes, [0, 5, 8]):
        name = f"06_noise_global_{sigma}"
        measured = metrics["scenarios"][name]["neurons"][0]
        voltage(axis, name, window=(0., 2.), title=f"Общий σ={sigma}; {measured['spikes']} спайков за 5 с; seed=42")
    axes[-1].set_xlabel("Время, мс")
    save_figure(figure, "05_global_noise")
    figure, axes = plt.subplots(2, 1, figsize=(10, 5.6), constrained_layout=True, sharex=True)
    voltage(axes[0], "07_noise_individual", 1, (0., 2.), title="N1: наследует общий σ=5")
    voltage(axes[1], "07_noise_individual", 2, (0., 2.), color="#bd4f35", title="N2: индивидуальный σ=8 переопределяет общий σ=5")
    axes[-1].set_xlabel("Время, мс")
    save_figure(figure, "06_individual_noise")
    figure, axes = plt.subplots(1, 2, figsize=(10, 3.6), constrained_layout=True)
    noise = metrics["noise_replicates"]
    axes[0].errorbar([5, 8], [noise[s]["mean_rate_hz"] for s in ["5", "8"]],
                     yerr=[noise[s]["sd_rate_hz"] for s in ["5", "8"]], fmt="o-", capsize=5, color="#2563a6")
    for sigma in [5, 8]:
        axes[0].scatter(np.full(5, sigma) + np.linspace(-.12, .12, 5), noise[str(sigma)]["rates_hz"], s=15, alpha=.4, color="#2563a6")
    axes[0].set(xlabel="σ, ед. тока", ylabel="Частота, Гц", title="Среднее ± SD по 5 seed", xticks=[5, 8])
    axes[1].bar(["5", "8"], [noise[s]["mean_isi_cv"] or 0. for s in ["5", "8"]], color=["#2563a6", "#bd4f35"], width=.5)
    axes[1].set(xlabel="σ, ед. тока", ylabel="Средний CV межспайковых интервалов", title="Нерегулярность, окно 1–10 с")
    save_figure(figure, "07_noise_statistics")


def fmt(value, digits=3):
    return "—" if value is None else f"{value:.{digits}f}"


def report(metrics, cli):
    m = metrics["scenarios"]
    periodic_rows = "\n".join(f"| {hz} | {metrics['checks']['periodic'][str(hz)]['pulses']} | {metrics['checks']['periodic'][str(hz)]['spikes']} | {metrics['checks']['periodic'][str(hz)]['isi_frequency_hz']:.4f} |"
                              for hz in [2, 10, 20])
    convergence_rows = "\n".join(f"| `{name}` | {v['coarse_spikes']} / {v['fine_spikes']} | {v['max_event_time_difference_ms']:.4f} |"
                                 for name, v in metrics["convergence"].items())
    noise_rows = "\n".join(f"| {sigma} | {v['mean_rate_hz']:.3f} ± {v['sd_rate_hz']:.3f} | {fmt(v['mean_isi_cv'])} |"
                           for sigma, v in metrics["noise_replicates"].items())
    tonic = m["03_autonomous"]["neurons"][0]
    dc = m["05_dc_4"]["neurons"][0]
    reference_single = m["02_reference_30x3ms"]["neurons"][0]["spikes"]
    reference_train = m["04_reference_10hz_30x3ms"]["neurons"][0]["spikes"]
    firing = [row["current"] for row in metrics["dc_sweep"] if row["analyzed_spikes"] >= 3]
    threshold = min(firing)
    lower = max(row["current"] for row in metrics["dc_sweep"] if row["current"] < threshold)
    content = f"""# Лабораторная работа 1: динамика нейрона Ижикевича

Выполнены четыре задания из учебного пособия `UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf`, страницы 12–17; уравнения модели и синапса — страницы 5–7. Численные данные основной серии получены исполняемым файлом `network_studio_cli` с тем же C++ ядром, которое использует графический конфигуратор ImGui/ImPlot. Скрипт `reproduce.py` формирует конфигурации, проверяет CSV и строит изображения. Независимые проверки решателей вынесены в отдельные скрипты.

Контрольный опыт в оригинальном **NeuroNet SM R05** объяснил расхождение для периодической стимуляции **30 × 3 мс, 10 Гц**: при `Step=0.500 мс` на Em виден один спайк на импульс, при `Step=0.025 мс` — два, при остальных сохранённых настройках. В этой версии используется явный Эйлер; Network Studio использует RK4 с шагом 0.025 мс. Таким образом, сравнивались разные численные протоколы. Скриншоты пользователя, воспроизводимый расчёт и ограничения сравнения: [проверка NeuroNet](results/neuronet_comparison/README.md). Длительность 1 мс в основной серии ниже — отдельно подобранный стимул для получения одиночных ответов; это изменение исходного протокола, а не буквальное повторение опыта 30 × 3 мс.

## Повторный запуск

Из каталога `network_studio` после сборки:

```powershell
.\\build.ps1 -Test
python .\\lab1\\reproduce.py
```

Нужны Python 3, NumPy и Matplotlib, установленные для той же версии Python. Проверенная здесь среда: Python {sys.version.split()[0]}, NumPy {np.__version__}, Matplotlib {__import__('matplotlib').__version__}. Системный Python 3.13 не совместим с подготовленными в этой рабочей папке пакетами для CPython 3.12. Для повторения именно в текущем окружении из каталога `network_studio`:

```powershell
$labPython = Join-Path $env:USERPROFILE '.cache\\codex-runtimes\\codex-primary-runtime\\dependencies\\python\\python.exe'
& $labPython .\\lab1\\reproduce.py --python-deps '..\\..\\..\\tmp\\python_deps'
```

Путь с дополнительными пакетами подключается только явным ключом `--python-deps`; в обычной установке он не нужен. Альтернативный CLI: `python lab1/reproduce.py --cli path/to/network_studio_cli.exe`. Ключ `--prepare-only` обновляет конфигурации без вычислений, `--skip-run` заново проверяет и визуализирует уже существующие CSV. Для независимой проверки `reference_check.py` дополнительно нужна SciPy; в указанной среде её можно подключить через `PYTHONPATH` к той же папке пакетов.

Каждый JSON из [configs](configs) можно открыть в Network Studio через «Открыть». Для задания 1 выберите `02_single_pulse.json`, для задания 2 — `03_autonomous.json`, для задания 3 — `04_periodic_02hz.json`, `04_periodic_10hz.json`, `04_periodic_20hz.json`, `05_dc_4.json`, для задания 4 — `06_noise_global_*.json` и `07_noise_individual.json`. Нажмите запуск; напряжение видно на вкладке потенциалов, синаптический выход — на вкладке синапсов.

## Модель и договорённости

Уравнения Ижикевича: `dv/dt = 0.04 v² + 5 v + 140 − u + I`, `du/dt = a(bv−u)`. При `v≥30 мВ`: `v←c`, `u←u+d`. В этих уравнениях время измеряется в миллисекундах; ядро хранит секунды и переводит производные множителем 1000. RS: `a=0.02`, `b=0.2`, `c=−65`, `d=8`, `v0=−65`, `u0=b·v0`.

Шаг RK4 — 25 мкс. Напряжение записывается через 100 мкс (одиночный импульс — 50 мкс); момент спайка фиксируется на границе шага. Треугольники на графиках отмечают зарегистрированные события на уровне 30 мВ: пиковое значение после сброса не является состоянием, хранящимся в CSV. Кривые отображают именно записанное состояние. Имена CSV содержат физические единицы, время в CSV — секунды; на рисунках оно переведено в миллисекунды.

Ток измеряется в единицах модели. Методичка называет амплитуду стимулятора «Voltage/мВ», хотя подставляет её в `I` уравнения (1). Рекомендация страницы 16 — прямоугольный импульс 30 продолжительностью 3 мс — проверена отдельными сценариями. В RK4 с указанным шагом она вызывает дублет, что подтверждено независимыми адаптивными решателями. Контроль в NeuroNet показал зависимость числа спайков от шага при периоде 100 мс: одиночный ответ при 0.5 мс и двойной при 0.025 мс. Вывод об ошибочности рекомендации пособия на основании одного сравнения был бы преждевременным. Для одиночного ответа и частот 2/10/20 Гц в основной серии подобрана длительность **1 мс при той же амплитуде 30**. Задание 3 предлагает изменять параметры стимуляции для получения нужной активности. В иллюстрации страницы 15 и описании на странице 16 для другого, 30-миллисекундного примера значения 30 и 20 расходятся; этот неоднозначный пример не использован.

Синапс Цодыкса–Маркрамa: `dx/dt=z/τrec`, `dy/dt=−y/τI`, `du/dt=−u/τfacil`, `z=1−x−y`. При спайке сначала `u←u+U(1−u)`, затем `r=u·x_old`, `x←x−r`, `y←y+r`, как уточнено на странице 23. Ток связи `I_syn=w·g·y`. Для задания 1: `U=0.5`, `τrec=50 мс`, `τI=10 мс`, `τfacil=1000 мс`, `g=20`. Ресурсы нормированы: `x0=1,y0=z0=u0=0`. Это соответствует масштабу `Initial x=20` на скриншоте страницы 13, вынесенному в множитель `g`. Связь с пассивным вторым нейроном имеет малый вес `w=0.01`; она позволяет наблюдать выход и не меняет динамику источника. Астроциты и STDP в ЛР1 не участвуют.

## Задание 1: покой, одиночный спайк и выход

Без внешнего тока RS не генерирует спайков и приближается к устойчивому потенциалу −70 мВ. Подобранный импульс 100–101 мс вызывает ровно один спайк. Синаптический ответ после события затухает с `τI=10 мс`; начальный скачок `g·U·x0=10` единиц тока. Отдельная проверка одиночного импульса 100–103 мс методом RK4 даёт {reference_single} спайка: [02_reference_30x3ms.json](configs/02_reference_30x3ms.json). Это ответ на один стимул после покоя; его нельзя напрямую отождествлять с установившимся ответом на периодическую стимуляцию, поскольку переменная восстановления `u` зависит от предыдущих спайков.

![Покой, одиночный спайк и синаптический выход](results/figures/01_rest_single_output.png)

## Задание 2: автоколебания без внешнего тока

Изменён только `b`: `0.20→0.27`, `I=0`, шум выключен. Получено {tonic['spikes']} спайков за 5 с; частота по числу событий в окне 1–5 с — {tonic['rate_hz']:.3f} Гц, частота по среднему ISI — {fmt(tonic['isi_frequency_hz'])} Гц. Это наш выбранный набор параметров: методичка требует подобрать режим, но конкретных значений не приводит. Уравнение стационарного состояния `0.04v²+(5−b)v+140=0` при `b=0.27` не имеет вещественных корней; наблюдаемая повторяющаяся генерация реализуется вместе с правилом сброса.

![Автоколебания](results/figures/02_autonomous.png)

## Задание 3: 2, 10, 20 Гц и постоянный ток

Импульсы подаются с 100 мс в течение 3 с, подобранная ширина 1 мс, амплитуда 30, остальные параметры RS. В каждом периоде зарегистрирован один спайк. Для сравнения, исходная ширина 3 мс при 10 Гц в RK4 вызывает {reference_train} спайков на 30 стимулов: [04_reference_10hz_30x3ms.json](configs/04_reference_10hz_30x3ms.json). Это согласуется по числу пиков на импульс с новым ручным опытом в NeuroNet при шаге 0.025 мс; точное совпадение времён событий между программами здесь не установлено.

| Заданная частота, Гц | Импульсов | Спайков | Частота по среднему ISI, Гц |
|---:|---:|---:|---:|
{periodic_rows}

![Периодическая стимуляция](results/figures/03_periodic_2_10_20hz.png)

При постоянном `I=4` получено {dc['spikes']} спайков за 5 с; установившаяся частота по числу событий за 1–5 с — {dc['rate_hz']:.3f} Гц. В выполненной сетке значений токов первая точка с устойчивой повторяющейся генерацией — `{threshold:g}`; предыдущая проверенная точка `{lower:g}` такого режима не дала. Это численная граница выбранного протокола (5 с, отбрасывание первой секунды, не менее трёх поздних спайков), а не точное доказательство порога бифуркации. Полная таблица: [dc_sweep.csv](results/dc_sweep.csv).

![Постоянный ток](results/figures/04_dc_sweep.png)

## Задание 4: глобальный и индивидуальный шум

Восстановлены параметры RS, постоянный ток и стимуляция выключены. Для каждого нейрона создаётся независимый гауссовский ток с нулевым средним. Новое значение получается каждые 1 мс и удерживается до следующего интервала; это дискретная аппроксимация шумового тока, **не непрерывная стохастическая дифференциальная модель**. Seed и индекс нейрона определяют воспроизводимую последовательность. Методичка не задаёт шаг или алгоритм шума, поэтому значения σ нельзя количественно отождествлять с частотами на скриншотах NeuroNet.

При общем `σ=0` спайков нет. Для примеров `σ=5` и `σ=8` появляются случайные спайки; увеличение σ повышает среднюю частоту в серии реализаций. На рисунках показаны первые 2 с пятисекундных записей, seed=42.

![Общий шум](results/figures/05_global_noise.png)

В отдельной двухнейронной конфигурации N1 наследует общий `σ=5`, N2 использует индивидуальный `σ=8`. Связей между ними нет. Трасса N1 побитово совпадает с однонейронным случаем глобального `σ=5`: добавление другого нейрона не меняет его поток случайных чисел. Повторный запуск всей конфигурации также дал идентичные CSV состояний и событий.

![Индивидуальный шум](results/figures/06_individual_noise.png)

Для оценки разброса выполнено по пять независимых запусков по 10 с с seed `11,23,42,71,101`; статистика считается после первой секунды. Частота — число событий / 9 с; CV — стандартное отклонение межспайковых интервалов / средний интервал. SD между запусками показывает разброс, не доверительный интервал.

| σ | Частота, среднее ± SD, Гц | Средний CV ISI |
|---:|---:|---:|
{noise_rows}

![Статистика шумовых реализаций](results/figures/07_noise_statistics.png)

## Проверки и пределы сравнения

Для всех запусков проверены конечность состояний, строгий рост времени, достижение конечного времени и совпадение количества событий с итоговым счётчиком. Проверены: покой без входа, один спайк на одиночный импульс, один спайк на каждый период 2/10/20 Гц, длительная генерация при `b=0.27` и при `I=4`, наследование глобального шума и повторяемость seed.

Повторение детерминистических опытов с шагом 12.5 мкс сохраняет количество спайков. Для соответствующих порядковых событий:

| Опыт | Спайки при 25 / 12.5 мкс | Максимальная разница времени, мс |
|---|---:|---:|
{convergence_rows}

Полные машинные результаты: [metrics.json](results/metrics.json), сценарии и SHA-256 исходного PDF: [manifest.json](results/manifest.json). Хеши исполняемого файла, исходников и конфигураций, а также версии Python-пакетов: [provenance.json](results/provenance.json). Скрипт проверяет совпадение экспортированного снимка каждого опыта с запрошенной конфигурацией; при `--skip-run` другие параметры не будут выданы за уже рассчитанные. CSV всех состояний, события, точные снимки конфигураций и журналы CLI создаются в `results/raw/`; этот объёмный воспроизводимый каталог исключён из Git. Сводки, параметры и рисунки сохраняются в репозитории.

Дополнительно тот же CLI сопоставлен с независимым адаптивным решателем `scipy.integrate.solve_ivp`, обнаруживающим пересечение порога и перезапускающим интегрирование после сброса. Сценарии включают 30×3 мс, 30×1 мс на 2/10/20 Гц, автономный `b=0.27` и постоянный `I=4`. Проверка подтверждает дублеты при 3 мс, одинаковое количество событий и уменьшение временной ошибки при сокращении шага. Воспроизведение: `python reference_check.py` из каталога `network_studio`; сохранённая сводка: [reference_summary.json](results/reference_summary.json).

Дополнительный [аудит численной схемы](results/audit_numerics/README.md) сравнивает C++ с DOP853 и Radau, проверяет дозу стимула, начальные состояния и уменьшение шага. Проверка [оригинального NeuroNet](results/neuronet_comparison/README.md) дополнительно учитывает периодическую стимуляцию и показания интерфейса. Поле Frequency в проверенной версии считает события в скользящем окне, по умолчанию 50 мс: один спайк даёт показание 20 Гц, два — 40 Гц. Это не средняя частота за секунду; число спайков проверяли по увеличенному Em.

В пособии нет исходных временных рядов и seed; на снимке интерфейса страницы 11 виден Step=0.500. Метод интегрирования установлен отдельно для проверенного локального NeuroNet SM R05 по бинарному файлу. Результаты повторного ручного опыта относятся к стимуляции 10 Гц и не доказывают количественного совпадения остальных заданий, шумовых реализаций или всех версий NeuroNet. Численные частоты и времена спайков основной серии относятся к приведённому протоколу Network Studio.
"""
    (HERE / "README.md").write_text(content, encoding="utf-8")
    reference_path = STUDIO / "verification" / "reference" / "reference_summary.json"
    if reference_path.is_file():
        write_json(RESULTS / "reference_summary.json", json.loads(reference_path.read_text(encoding="utf-8")))
    run_path = RESULTS / "run_provenance.json"
    provenance = json.loads(run_path.read_text(encoding="utf-8")) if run_path.is_file() else {}
    provenance.update(python=sys.version, numpy=np.__version__, matplotlib=__import__("matplotlib").__version__)
    write_json(RESULTS / "provenance.json", provenance)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=STUDIO / "build" / "Release" / "network_studio_cli.exe")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument("--python-deps", type=Path, help="Explicit NumPy/Matplotlib directory matching this Python ABI")
    args = parser.parse_args()
    scenarios, main_names, deterministic = prepare()
    if args.prepare_only:
        print(f"Prepared {len(scenarios)} configurations in {CONFIGS}")
        return
    if not args.skip_run:
        execute(args.cli.resolve(), scenarios)
    imports(args.python_deps)
    metrics = analyze(scenarios, main_names, deterministic)
    plots(metrics)
    report(metrics, args.cli.resolve())
    print(f"Verified {len(scenarios)} simulations; report: {HERE / 'README.md'}")


if __name__ == "__main__":
    main()
