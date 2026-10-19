"""Summarize completed, saved parallel-solver measurements; never execute a solver.

Only small JSON/XML reports are read. Numerical comparison is NOT rerun: the saved
comparison reports are checked for complete, internally consistent success flags.
With --write-report, regenerate the detailed draft MEASUREMENTS_RU.md appendix.
"""
import argparse
from collections import defaultdict
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import statistics
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CSV_NAMES = {"result_" + name + ".csv" for name in (
    "activity", "bursts", "deliveries", "events", "pending_deliveries", "responses",
    "shared_stimuli", "stimuli", "trace", "weights")}
BUILD_EXCLUSIONS = ["length-prefixed build ID", "dependent final integrity checksum"]


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def relative(path):
    path = Path(path).resolve()
    return path.relative_to(HERE).as_posix() if path.is_relative_to(HERE) else str(path)


def report_path(value, fallback):
    path = Path(value) if value else fallback
    return path if path.is_absolute() else ROOT / path


def check_comparison(path):
    data = read(path)
    problems = []
    if data.get("equal") is not True:
        problems.append("top-level equal is not true")
    csv = data.get("csv", {})
    if set(csv) != CSV_NAMES:
        problems.append("CSV inventory differs from the ten expected exports")
    for name, item in csv.items():
        if item.get("byte_equal") is not True:
            problems.append(name + ": byte_equal is not true")
        if not item.get("reference_sha256") or item.get("reference_sha256") != item.get("actual_sha256"):
            problems.append(name + ": missing or contradictory hashes")
    checkpoint = data.get("checkpoint", {})
    mode = data.get("checkpoint_mode")
    if mode == "exact":
        if checkpoint.get("checkpoint_files_byte_equal") is not True or checkpoint.get("excluded_fields") != []:
            problems.append("exact checkpoint success/exclusions differ")
        if not checkpoint.get("reference_sha256") or checkpoint.get("reference_sha256") != checkpoint.get("actual_sha256"):
            problems.append("exact checkpoint hashes missing or contradictory")
    elif mode == "build-metadata":
        for key in ("equal_except_build_metadata", "same_schema", "same_algorithm", "same_serialized_config_bytes",
                    "same_non_build_header_bytes", "same_complete_payload_bytes"):
            if checkpoint.get(key) is not True:
                problems.append("checkpoint " + key + " is not true")
        if checkpoint.get("excluded_fields") != BUILD_EXCLUSIONS:
            problems.append("undocumented checkpoint exclusions")
        reference, actual = checkpoint.get("reference", {}), checkpoint.get("actual", {})
        for key in ("schema", "algorithm", "config_sha256", "payload_sha256"):
            if reference.get(key) is None or reference.get(key) != actual.get(key):
                problems.append("checkpoint " + key + " missing or contradictory")
        if any(item.get("original_checksum_valid") is not True for item in (reference, actual)):
            problems.append("original integrity checksums were not both verified")
    else:
        problems.append("unknown checkpoint comparison mode")
    return {"path": relative(path), "valid": not problems, "mode": mode,
            "reference": data.get("reference"), "actual": data.get("actual"), "problems": problems}


def stats(values):
    values = [float(value) for value in values if value is not None]
    return {"n": len(values), "median": statistics.median(values), "min": min(values), "max": max(values),
            "sample_sd": statistics.stdev(values) if len(values) > 1 else None} if values else None


def unique(values):
    return sorted({value for value in values if value is not None})


def collect(selected):
    reports, rows, issues, sources = {}, [], [], []
    # Include every saved comparison, including early manually collected stages.
    for folder in selected:
        for path in sorted(folder.rglob("comparison.json")):
            try:
                result = check_comparison(path)
                reports[str(path.resolve())] = result
                if not result["valid"]:
                    issues.append({"path": relative(path), "problems": result["problems"]})
            except (OSError, ValueError, TypeError) as error:
                issues.append({"path": relative(path), "problems": [str(error)]})
    seen = set()
    for folder in selected:
        for manifest in sorted(folder.rglob("runs.json")):
            try:
                data = read(manifest)
            except (OSError, ValueError) as error:
                issues.append({"path": relative(manifest), "problems": ["Could be an in-progress write; rerun: " + str(error)]})
                continue
            sources.append({"path": relative(manifest), "binaries": data.get("binaries"),
                            "exe": data.get("exe"), "exe_sha256": data.get("exe_sha256"),
                            "limits": data.get("limits"), "started_utc": data.get("started_utc")})
            for saved in data.get("runs", []):
                row = dict(saved)
                output = report_path(row.get("output"), manifest.parent)
                row.update(series=relative(manifest.parent), mode=data.get("mode", "cli"), output=relative(output),
                           phase=data.get("phase", "crossover" if data.get("synthetic") else "unspecified"))
                row.setdefault("label", "threads_" + str(row.get("threads", "unknown")))
                row.setdefault("workload", "synthetic_N" + str(row.get("neurons", "unknown")))
                # Transmitted delta entries include boundary replacements and
                # are deliberately not treated as unique scientific events.
                if row["mode"] == "runner" and "events" not in row and "neuron_spikes" in row:
                    row["events"] = row["neuron_spikes"]
                    row["events_source"] = "neuron_spikes (final worker history), not delta_event_entries"
                row["instrumented"] = "profile" in str(manifest.parent).lower() or "profile" in str(data.get("binaries", {}).get("new", {}).get("path", "")).lower()
                row["reference_only"] = bool(row["label"] == "frozen" or (data.get("synthetic") and row.get("threads") == 1 and row.get("repeat") == 1))
                row["comparison"] = reports.get(str((output / "comparison.json").resolve()))
                if not row["reference_only"] and (not row["comparison"] or not row["comparison"]["valid"]):
                    issues.append({"path": relative(output), "problems": ["Committed run has no valid comparison report"]})
                if row.get("comparison_equal") is False:
                    issues.append({"path": relative(output), "problems": ["Manifest records comparison_equal=false"]})
                profile_path = output / "profile.json"
                if profile_path.is_file():
                    profile = read(profile_path)
                    if profile.get("enabled"):
                        row["instrumented"] = row["label"] != "frozen" or row["instrumented"]
                        row["profile"] = profile
                rows.append(row); seen.add(output.resolve())
    # These pre-matrix stages intentionally have no runs.json or memory samples.
    for folder in selected:
        for timing in sorted(folder.rglob("timing.json")):
            if timing.parent.resolve() in seen or not timing.relative_to(HERE).parts[0].startswith(("stage1_", "stage2_", "stage3_")):
                continue
            comparison = reports.get(str((timing.parent / "comparison.json").resolve()))
            if not comparison or not comparison["valid"]:
                issues.append({"path": relative(timing), "problems": ["Early stage has no valid completed comparison"]})
                continue
            row = read(timing)
            row.update(series=relative(timing.parent.parent), mode="cli", phase="early-stage", workload="burst",
                       output=relative(timing.parent), label="threads_" + str(row.get("requested_threads", "unknown")),
                       repeat=1, reference_only=False, comparison=comparison, instrumented=False)
            rows.append(row)
    return rows, list(reports.values()), issues, sources


def collect_integration(selected, comparisons, matrix_rows, issues):
    """Keep integration timers/partial CLI runs outside the throughput matrix."""
    reports = {item["path"]: item for item in comparisons}
    seen = {(HERE / row["output"]).resolve() for row in matrix_rows}
    manifests = set()
    series = []
    for folder in selected:
        for manifest in sorted(folder.rglob("integration.json")):
            if manifest.resolve() in manifests:
                continue
            manifests.add(manifest.resolve())
            try:
                data = read(manifest)
            except (OSError, ValueError) as error:
                issues.append({"path": relative(manifest), "problems": ["Could be an in-progress write; rerun: " + str(error)]})
                continue
            result = {"path": relative(manifest), "series": relative(manifest.parent),
                      "started_utc": data.get("started_utc"), "fixture": data.get("fixture"),
                      "fixture_sha256": data.get("fixture_sha256"), "model_duration": data.get("model_duration"),
                      "note": data.get("note"), "runs": []}
            for saved in data.get("runs", []):
                row = dict(saved)
                output = (manifest.parent / row["name"]).resolve()
                if output in seen:
                    issues.append({"path": relative(output), "problems": ["Duplicate output already counted; integration entry omitted"]})
                    continue
                seen.add(output)
                row["output"] = relative(output)
                timing = row.get("timing") or {}
                comparison = reports.get(relative(output / "comparison.json"))
                row["comparison"] = comparison
                row["role"] = ("reference_capture" if row["name"] == "direct_1" else
                               "partial_checkpoint_for_resume" if row["name"] == "cli_prefix_2" else "full_result_comparison")
                if row["role"] == "full_result_comparison":
                    if row.get("comparison_equal") is not True or not comparison or not comparison["valid"] or comparison["mode"] != "exact":
                        issues.append({"path": relative(output), "problems": ["Integration result lacks a valid exact same-build comparison"]})
                metric = "solve_seconds" if "solve_seconds" in timing else "wall_seconds" if "wall_seconds" in timing else "process_seconds"
                row["metric"] = metric
                row["measured_seconds"] = timing.get(metric) if metric != "process_seconds" else row.get("process_seconds")
                if not isinstance(row["measured_seconds"], (int, float)) or not math.isfinite(row["measured_seconds"]) or row["measured_seconds"] <= 0:
                    issues.append({"path": relative(output), "problems": ["Integration timer missing or invalid"]})
                row["requested_threads"] = timing.get("requested_threads", timing.get("compute_threads"))
                row["effective_threads"] = timing.get("effective_threads", timing.get("effective_compute_threads"))
                row["observed_model_final_seconds"] = timing.get("model_final_seconds", timing.get("model_seconds"))
                row["observed_model_start_seconds"] = timing.get("model_start_seconds", 0 if row["name"] == "direct_1" else None)
                if row["observed_model_final_seconds"] is not None:
                    declared_end = row.get("start_model_seconds", 0) + row.get("integrated_seconds", 0)
                    if row["observed_model_final_seconds"] != declared_end:
                        issues.append({"path": relative(output), "problems": ["Observed integration final time differs from declared end"]})
                    if timing.get("steps") != round(declared_end / .000025):
                        issues.append({"path": relative(output), "problems": ["Integration macro-step count differs from final time / 25us"]})
                if row["name"].startswith("runner_") and timing.get("final_snapshot_confirmed") is not True:
                    issues.append({"path": relative(output), "problems": ["Runner final snapshot not explicitly confirmed"]})
                # Do not infer effective CLI thread count from its argument.
                row["cli_time_scope"] = "Whole process including parse/load/export/checkpoint" if metric == "process_seconds" else None
                result["runs"].append(row)
            gui_serial = next((r for r in result["runs"] if r["name"] == "gui_1" and r["metric"] == "wall_seconds"), None)
            for row in result["runs"]:
                row["gui_speedup_vs_gui1"] = (gui_serial["measured_seconds"] / row["measured_seconds"]
                    if gui_serial and row["name"].startswith("gui_") and row["metric"] == "wall_seconds"
                    and isinstance(gui_serial["measured_seconds"], (int, float)) and isinstance(row["measured_seconds"], (int, float))
                    and row["measured_seconds"] > 0 else None)
            series.append(result)
    return series


def aggregate(rows, issues):
    grouped = defaultdict(list)
    for row in rows:
        duration = row.get("model_seconds", row.get("model_final_seconds"))
        metric = "solve_seconds" if row["mode"] == "cli" else "wall_seconds"
        if not isinstance(duration, (int, float)) or duration <= 0 or not isinstance(row.get(metric), (int, float)) or not math.isfinite(row[metric]) or row[metric] <= 0:
            issues.append({"path": row["output"], "problems": ["Missing/invalid duration or measured time"]}); continue
        if row.get("steps") != round(duration / .000025):
            issues.append({"path": row["output"], "problems": ["Recorded macro-step count differs from duration / 25us"]})
        if row["mode"] == "runner" and row.get("final_snapshot_confirmed") is not True:
            issues.append({"path": row["output"], "problems": ["Runner final snapshot not explicitly confirmed"]})
        key = (row["series"], row["mode"], row["workload"], duration, row["label"])
        grouped[key].append(row)
    groups = []
    for (series, mode, workload, duration, label), runs in grouped.items():
        metric = "solve_seconds" if mode == "cli" else "wall_seconds"
        memory = [r.get("memory", {}) for r in runs]
        groups.append({"series": series, "mode": mode, "workload": workload, "model_seconds": duration, "label": label,
                       "metric": metric, "time_seconds": stats(r[metric] for r in runs),
                       "steps": unique(r.get("steps") for r in runs), "events": unique(r.get("events") for r in runs),
                       "deliveries": unique(r.get("deliveries") for r in runs),
                       "requested_threads": unique(r.get("requested_threads", r.get("threads_requested", r.get("threads"))) for r in runs),
                       "effective_threads": unique(r.get("effective_threads") for r in runs),
                       "peak_working_set_mib": stats(m.get("peak_working_set_bytes_observed") / 2**20 for m in memory if m.get("peak_working_set_bytes_observed") is not None),
                       "max_sampled_private_mib": stats(m.get("max_sampled_private_usage_bytes") / 2**20 for m in memory if m.get("max_sampled_private_usage_bytes") is not None),
                       "memory_query_errors": [e for m in memory for e in m.get("query_errors", [])],
                       "instrumented": any(r.get("instrumented") for r in runs),
                       "numerical_comparisons": sum(r.get("comparison", {}).get("valid", False) for r in runs if r.get("comparison")),
                       "reference_captures": sum(r["reference_only"] for r in runs),
                       "outputs": [r["output"] for r in runs]})
    index = {(g["series"], g["mode"], g["workload"], g["model_seconds"], g["label"]): g for g in groups}
    for group in groups:
        prefix = (group["series"], group["mode"], group["workload"], group["model_seconds"])
        for label, name in (("threads_1", "speedup_vs_new1"), ("frozen", "speedup_vs_frozen")):
            baseline = index.get(prefix + (label,))
            group[name] = baseline["time_seconds"]["median"] / group["time_seconds"]["median"] if baseline else None
        group["linear_150_model_seconds_estimate"] = (150 * group["time_seconds"]["median"] / group["model_seconds"]
            if group["model_seconds"] >= .5 and group["workload"] == "burst" and not group["instrumented"] else None)
    return sorted(groups, key=lambda g: (g["series"], g["mode"], g["workload"], g["model_seconds"], g["label"]))


def number(value, digits=3):
    return "—" if value is None else f"{value:.{digits}f}"


def values(items):
    return ", ".join(str(item) for item in items) if items else "—"


def render(summary):
    lines = ["# Параллельный решатель: предварительный отчёт по сохранённым измерениям", "",
        "Автоматически сформированный черновик. Содержит только завершённые записи на момент " + summary["generated_utc"] + ". Последующие серии и окончательный архив сборки могут ещё отсутствовать.", "",
        "`summarize.py` не запускает расчёты и не перечитывает крупные численные файлы: он проверяет согласованность сохранённых `comparison.json` и сводит `runs.json`/ранние `timing.json`. Повторную проверку самих CSV/checkpoint выполняют команды методики.", "",
        "## Конфигурация и условия", "",
        "Основная сеть: 500 нейронов Ижикевича (400 возбуждающих, 100 тормозных), 15 013 TM-синапсов, мультипликативная STDP, seed 706, шаг 25 мкс, интервал шума 1 мс. Это документированная производная сохранённого опыта, а не утверждение о точном совпадении с упомянутой сетью из примерно 15 041 связи. `burst` сохраняет σ=5; `quiet` — синтетический контроль с σ=0. Происхождение и SHA конфигураций: [provenance.json](provenance.json).", "",
        "Синтетические малые сети crossover, если представлены ниже, имеют 64/128/256 нейронов и 2048/4096/8192 связей; их результаты не являются воспроизведением лабораторной. См. [crossover_provenance_v2.json](crossover_provenance_v2.json). Первая версия fixture не прошла разбор из-за отсутствующих полей пустого раздела stimulators, до численного расчёта; её файлы и неудачная попытка сохранены отдельно.", ""]
    env = summary.get("environment", {})
    lines += [f"Оборудование: {env.get('cpu', {}).get('ProcessorNameString', 'не записано')}; логических процессоров: {env.get('logical_processors', '—')}; RAM: {number(env.get('total_physical_bytes', 0) / 2**30, 2)} GiB. ОС: {env.get('platform', '—')}. План питания: {env.get('power_scheme_guid', '—')}. Температура и фоновая нагрузка не контролировались; порядок потоков фиксирован, процессы запускаются последовательно.", "",
        "`solve_seconds` — только прямой расчёт CLI, `wall_seconds` runner — до подтверждённого финального snapshot с затратами публикации/приёма. Эти метрики не смешиваются. Таблицы этого этапа сами по себе не доказывают ускорения реального GUI. Источники, записанные SHA исполняемых файлов и команды каждого запуска сохранены в `summary.json`; SHA исполняемого файла не заменяет отсутствующий архив исходников раннего этапа.", "",
        "## Измеренные серии", "",
        "Время — медиана; SD — выборочное стандартное отклонение (при n=1 не определено). Ускорение = медиана соответствующего эталона / текущая медиана в той же серии, конфигурации и длительности. Столбцы new1 и frozen используют разные эталоны и не взаимозаменяемы. `Auto → k` — фактически зарегистрированное число вычисляющих потоков, включая координатор.", ""]
    last_series = None
    for g in summary["groups"]:
        if g["series"] != last_series:
            lines += ["", "### " + g["series"], "", "| Нагрузка / метод | Модель, с | Потоки запрос→факт | n | Медиана, с | min–max, с | SD, с | × new1 | × frozen | Шаги / события / доставки | WS peak MiB¹ | private sampled MiB¹ |", "|---|---:|---|---:|---:|---|---:|---:|---:|---|---|---|"]
            last_series = g["series"]
        requested = "Auto" if g["requested_threads"] == [0] else values(g["requested_threads"])
        threads = "frozen" if g["label"] == "frozen" else requested + "→" + values(g["effective_threads"])
        t = g["time_seconds"]
        mem = lambda m: "—" if not m else number(m["median"], 2) + " / " + number(m["max"], 2)
        lines.append(f"| {g['workload']} / {g['mode']}{' †' if g['instrumented'] else ''} | {g['model_seconds']:g} | {threads} | {t['n']} | {number(t['median'])} | {number(t['min'])}–{number(t['max'])} | {number(t['sample_sd'])} | {number(g['speedup_vs_new1'], 2)} | {number(g['speedup_vs_frozen'], 2)} | {values(g['steps'])} / {values(g['events'])} / {values(g['deliveries'])} | {mem(g['peak_working_set_mib'])} | {mem(g['max_sampled_private_mib'])} |")
    lines += ["", "¹ Память: медиана / максимум среди запусков. `PeakWorkingSetSize` — максимум ОС до последнего успешного опроса; `PrivateUsage` — максимум выборки (интервал в `memory.json`), который может пропустить пики. Измеряется весь дочерний процесс, включая инициализацию, экспорт и checkpoint; это не память одного ядра расчёта. Отсутствующее измерение обозначено «—». † Профилирование включено либо серия предназначена для профилирования; её время отделено от Release-серий без счётчиков. Для runner события берутся из итогового `neuron_spikes`, а не из числа переданных delta-записей; отсутствующий полный счётчик доставок не заменяется транспортной статистикой.", "",
        "Начальный интервал 5 мс нельзя считать установившейся нагрузкой или переносить его ускорение на длительный опыт. Число событий в таблице фактическое: ранний `stage4_smoke/burst` содержит 6 спайков и 161 доставку; интервал 50 мс содержит 5771 спайк и 172 612 доставок. У quiet на 5 мс — 0/0. Название `burst` описывает рецепт; число сетевых пачек этим счётчиком не измеряется.", "",
        "## Проверка численной идентичности", "",
        f"Найдено {len(summary['comparisons'])} отчётов сравнения; успешных и внутренне согласованных: {sum(c['valid'] for c in summary['comparisons'])}. Проблемы сбора/проверки: {len(summary['issues'])}. Эталонные запуски frozen и первый serial crossover являются захватом эталона, а не сравнением с самими собой.", "",
        "Каждое успешное сравнение требует всех 10 CSV побайтово. Для одной сборки дополнительно требуется побайтовое равенство целого checkpoint без исключений. Между сборками разрешены только length-prefixed build ID и зависимая финальная контрольная сумма; исходные суммы проверены, схема, алгоритм, сериализованная конфигурация, все остальные заголовки и полный payload должны совпасть. Это офлайн-сравнение не обходит compatibility и не разрешает загрузку несовместимого checkpoint.", ""]
    if summary["issues"]:
        lines += ["Обнаруженные проблемы (до устранения нельзя заключать, что все проверки успешны):", ""]
        lines += ["- `" + i["path"] + "`: " + "; ".join(i["problems"]) for i in summary["issues"]]
        lines += [""]
    lines += ["## Отдельная интеграционная проверка GUI / runner / CLI", "",
        "Эти записи взяты из `integration.json` и не включены повторно в матрицу выше. Direct измеряет solve time, GUI/runner — собственный wall timer, CLI prefix/resume — весь процесс с загрузкой, экспортом и checkpoint. Ускорение GUI считается только относительно gui_1 той же серии; сравнение его таймера с solve time не выдаётся за ускорение одного и того же участка. Здесь по одному запуску каждого варианта, разброс не измерен.", ""]
    if not summary["integration_series"]:
        lines += ["Завершённые записи integration.json пока отсутствуют.", ""]
    for integration in summary["integration_series"]:
        lines += ["### " + integration["series"], "",
                  "| Запуск | Таймер | Время, с | Модель начало→конец, с | Запрос→факт | × gui_1 | Численное сравнение |",
                  "|---|---|---:|---|---|---:|---|"]
        for row in integration["runs"]:
            requested = "Auto" if row["requested_threads"] == 0 else str(row["requested_threads"]) if row["requested_threads"] is not None else "—"
            effective = str(row["effective_threads"]) if row["effective_threads"] is not None else "—"
            interval = number(row["observed_model_start_seconds"]) + "→" + number(row["observed_model_final_seconds"])
            if row["observed_model_final_seconds"] is None:
                interval = "задано " + number(row.get("start_model_seconds")) + "→" + number(row.get("start_model_seconds", 0) + row.get("integrated_seconds", 0))
            comparison = ("эталон" if row["role"] == "reference_capture" else "prefix для resume" if row["role"] == "partial_checkpoint_for_resume" else
                          "exact ✓" if (row.get("comparison") or {}).get("valid") else "НЕ ПОДТВЕРЖДЕНО")
            lines.append(f"| {row['name']} | {row['metric']} | {number(row['measured_seconds'])} | {interval} | {requested}→{effective} | {number(row['gui_speedup_vs_gui1'], 2)} | {comparison} |")
        lines += ["", "| Запуск | OS peak WS MiB | Sampled private MiB | Hidden / renderer | Кадры | Present ok / occluded / other |", "|---|---:|---:|---|---:|---|"]
        for row in integration["runs"]:
            memory, timing = row.get("memory", {}), row.get("timing") or {}
            peak = memory.get("peak_working_set_bytes_observed")
            private = memory.get("max_sampled_private_usage_bytes")
            display = str(timing.get("hidden", "—")) + " / " + str(timing.get("renderer", "—"))
            present = " / ".join(str(timing.get(k, "—")) for k in ("present_ok", "present_occluded", "present_other_status"))
            lines.append(f"| {row['name']} | {number(peak / 2**20 if peak is not None else None, 2)} | {number(private / 2**20 if private is not None else None, 2)} | {display} | {timing.get('frames', '—')} | {present} |")
        lines += ["", "Скрытое окно Win32/DX11 с Present(1) действительно проходит native GUI loop. Это не ручная сессия видимого приложения и не GPU timer; окклюзия/ожидания могут влиять на кадры. Полные времена draw/render/present и worker, частота публикации, команды и SHA EXE сохранены в JSON. CLI prefix не сравнивается с полным интервалом, но штатный loader использует его для следующего resume; завершённый resume сравнивается с direct_1 полностью.", ""]
    lines += ["## Профиль и существующие тесты", ""]
    profiles = [r for r in summary["runs"] if r.get("profile")]
    if profiles:
        lines += ["Включённые профили (exclusive wall time в записанных категориях; включённые категории могут охватывать работу/ожидание группы потоков, это не сумма CPU-времени всех workers):", "", "| Запуск | Категория | Вызовы | Exclusive, с | Inclusive, с |", "|---|---|---:|---:|---:|"]
        for row in profiles:
            for category in sorted(row["profile"].get("categories", []), key=lambda c: c.get("exclusive_seconds", 0), reverse=True)[:6]:
                lines.append(f"| {row['output']} | {category['name']} | {category['calls']} | {number(category['exclusive_seconds'])} | {number(category['inclusive_seconds'])} |")
        lines += [""]
    else:
        lines += ["Завершённые измерения с включённым профилем пока не найдены.", ""]
    for test in summary["test_reports"]:
        lines.append(f"- `{test['path']}`: tests={test.get('tests', '—')}, failures={test.get('failures', '—')}, errors={test.get('errors', '0')}, skipped={test.get('skipped', '—')}; это сохранённый отчёт своего этапа, не заявление о текущем состоянии всех тестов.")
    lines += ["", "## Оценка для 150 модельных секунд", "",
        "Расчёт на 150 модельных секунд в этих сериях не выполнялся. Формальная линейная оценка: `150 × median_wall / measured_model_duration`. Её можно использовать только как условный ориентир по длиннейшему проверенному интервалу, не как обещание времени выполнения. Меняются спайковая активность, веса, плотность локализации событий, размеры истории и поведение скользящих журналов активности; это способно сделать экстраполяцию неверной.", ""]
    estimates = [g for g in summary["groups"] if g["linear_150_model_seconds_estimate"] is not None]
    if estimates:
        lines += ["| Серия | Режим | Измеренный интервал, с | Условные часы на 150 с (не измерены) |", "|---|---|---:|---:|"]
        for g in estimates:
            lines.append(f"| {g['series']} | {g['label']} / {g['mode']} | {g['model_seconds']:g} | {number(g['linear_150_model_seconds_estimate'] / 3600, 2)} |")
    else:
        lines += ["Завершённой непрофилируемой burst-серии длительностью не менее 0,5 с пока нет; численная оценка из smoke-интервала намеренно не приводится."]
    lines += ["", "## Воспроизведение и ограничения", "",
        "Методика и команды запуска: [README_RU.md](README_RU.md). Устройство параллельных фаз: [ARCHITECTURE_RU.md](ARCHITECTURE_RU.md). Команды каждого измерения доступны в исходных `runs.json` и `summary.json`. Полный matrix не запускается автоматически этим отчётом.", "", "```powershell", "python -B performance/20261019_parallel/summarize.py --write-report", "```", "",
        "Для финального вывода требуется завершить выбранные повторные серии, проверить их реальные события/длительность, сохранить окончательную Release-сборку и соответствующие исходники. Ранние stages показывают ход оптимизации; при n=1 не характеризуют разброс. Сохранённые SHA относятся только к указанным артефактам. GUI, длительный 150-секундный режим и другие типы сети нельзя считать ускоренными на основании одних CLI-таблиц.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--series", nargs="+", help="Optional subdirectories under this folder; default discovers all completed series")
    parser.add_argument("--out", type=Path, default=HERE / "summary.json")
    parser.add_argument("--write-report", action="store_true", help="Regenerate the detailed draft MEASUREMENTS_RU.md appendix (never REPORT_RU.md)")
    args = parser.parse_args()
    selected = [(HERE / value).resolve() for value in args.series] if args.series else [HERE]
    if any(not path.is_dir() or not path.is_relative_to(HERE) for path in selected):
        parser.error("--series must name existing directories inside this experiment")
    rows, comparisons, issues, sources = collect(selected)
    integration = collect_integration(selected, comparisons, rows, issues)
    groups = aggregate(rows, issues)
    setup_failures = []
    failed_v1 = HERE / "crossover/neurons_64/threads_1/run_1"
    if (failed_v1 / "process.log").is_file() and not (failed_v1 / "timing.json").exists():
        setup_failures.append({"path": relative(failed_v1), "phase": "configuration_parse_before_solve",
                               "log": (failed_v1 / "process.log").read_text(encoding="utf-8-sig").strip(),
                               "included_in_measurements": False,
                               "resolution": "V2 adds the two required shared-stimulator fields; original failed artifacts preserved. See crossover_validation_v2/validation.json."})
    tests = []
    for path in sorted(HERE.glob("ctest*.xml")):
        try:
            tests.append(dict(ET.parse(path).getroot().attrib, path=relative(path)))
        except (OSError, ET.ParseError) as error:
            issues.append({"path": relative(path), "problems": [str(error)]})
    summary = {"generated_utc": datetime.now(timezone.utc).isoformat(), "status": "draft_completed_records_only",
               "selection": [relative(p) for p in selected], "environment": read(HERE / "environment.json"),
               "sources": sources, "groups": groups, "comparisons": comparisons, "issues": issues,
               "integration_series": integration,
               "setup_failures": setup_failures,
               "all_saved_comparison_reports_valid": bool(comparisons) and all(c["valid"] for c in comparisons),
               "test_reports": tests, "runs": rows,
               "limits": "No solver or numerical comparator executed. Saved JSON success/hashes checked internally, not rehashed from artifacts. In-progress runs without manifest commit are omitted. Stage runs kept distinct. No 150-model-second experiment performed."}
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.write_report:
        (HERE / "MEASUREMENTS_RU.md").write_text(render(summary), encoding="utf-8")
    print(json.dumps({"runs": len(rows), "integration_runs": sum(len(s["runs"]) for s in integration), "groups": len(groups), "comparisons": len(comparisons),
                      "issues": issues, "output": str(args.out)}, ensure_ascii=False))
    return 1 if issues else 0


if __name__ == "__main__":
    raise SystemExit(main())
