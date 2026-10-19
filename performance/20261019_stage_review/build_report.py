"""Build the stage-review report from completed saved evidence; never run a model.

Source-only tooling. After measured/runs.json is complete and CTest finishes:
  python -B performance/20261019_stage_review/build_report.py --write
No export/EXE hashes are recomputed. Saved comparison reports are validated.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import statistics
import sys
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "20261019_parallel"))
from summarize import check_comparison

DIRECT = ("s0_baseline", "s1_caches", "s2_compact", "s3_runner_direct", "p1_resources4", "p2_currents4", "p3_stdp4", "control_no_roots4", "p5_final4", "p5_final8")
PROFILE = ("profile_stage4_4", "profile_final4")
GUI = ("gui_baseline", "gui_compact", "gui_runner", "gui_final8")
TITLES = {"s0_baseline": "S0: исходный serial", "s1_caches": "S1: кэши", "s2_compact": "S2: компактное ядро",
          "s3_runner_direct": "S3: ядро версии с runner", "p1_resources4": "P1: TM-ресурсы, 4 потока",
          "p2_currents4": "P2: токи, 4 потока", "p3_stdp4": "P3: STDP, 4 потока",
          "control_no_roots4": "Новая абляция: serial roots, 4 потока", "p5_final4": "Финальная реализация, 4 потока",
          "p5_final8": "Финальная реализация, 8 потоков", "profile_stage4_4": "Исторический profile stage4, 4 потока",
          "profile_final4": "Финальный profile, 4 потока", "gui_baseline": "GUI исходный", "gui_compact": "GUI compact",
          "gui_runner": "GUI с runner", "gui_final8": "GUI final8"}
PARENT = {"s1_caches": "s0_baseline", "s2_compact": "s1_caches", "s3_runner_direct": "s2_compact",
          "p1_resources4": "s3_runner_direct", "p2_currents4": "p1_resources4", "p3_stdp4": "p2_currents4",
          "p5_final4": "control_no_roots4", "p5_final8": "p5_final4", "profile_final4": "profile_stage4_4",
          "gui_compact": "gui_baseline", "gui_runner": "gui_compact", "gui_final8": "gui_runner"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def stat(values):
    values = list(values)
    require(values and all(isinstance(v, (int, float)) and math.isfinite(v) for v in values), "Missing/nonfinite statistic")
    return {"n": len(values), "median": statistics.median(values), "min": min(values), "max": max(values),
            "sample_sd": statistics.stdev(values) if len(values) > 1 else None}


def number(value, digits=3):
    return "—" if value is None else f"{value:.{digits}f}"


def resolve(value):
    path = Path(value)
    return path if path.is_absolute() else ROOT / path


def check_saved_tests(path):
    xml = ET.parse(path).getroot()
    count = int(xml.get("tests", 0))
    failures = [{"name": c.get("name"), "output": (c.findtext("system-out") or "").strip()}
                for c in xml.findall("testcase") if c.find("failure") is not None or c.find("error") is not None]
    return {"path": str(path), "tests": count, "failures_count": int(xml.get("failures", 0)),
            "errors": int(xml.get("errors", 0)), "skipped": int(xml.get("skipped", 0)),
            "disabled": int(xml.get("disabled", 0)), "timestamp": xml.get("timestamp"), "failures": failures}


def historical_rows():
    result = []
    for name, path, metric in (
        ("Исходный serial", "20261019_single_thread/before/runs.json", "solve_seconds"),
        ("Оптимизированный serial", "20261019_single_thread/after/runs.json", "solve_seconds"),
        ("Runner с serial core", "20261019_runner/runner_full/runs.json", "wall_seconds")):
        data = read(HERE.parent / path)
        rows = data["runs"]
        require(all(r.get("model_seconds", r.get("model_final_seconds")) == .5 for r in rows), "Historical duration changed")
        result.append({"name": name, "path": "../" + path, "metric": metric, "model_seconds": .5,
                       "time": stat(r[metric] for r in rows)})
    path = HERE.parent / "20261019_parallel/final_500ms/runs.json"
    data = read(path)
    for label in ("threads_1", "threads_2", "threads_4", "threads_8", "threads_0"):
        rows = [r for r in data["runs"] if r["label"] == label and r["workload"] == "burst"]
        require(rows and all(r["model_seconds"] == .5 for r in rows), "Historical parallel duration changed")
        result.append({"name": "Финальный " + label.replace("threads_0", "Auto→8").replace("threads_", "threads="),
                       "path": "../20261019_parallel/final_500ms/runs.json", "metric": "solve_seconds", "model_seconds": .5,
                       "time": stat(r["solve_seconds"] for r in rows)})
    return result


def phase_profiles(rows):
    """Aggregate saved coordinator wall timers without adding nested scopes."""
    result = {}
    for label in PROFILE:
        selected = [r for r in rows if r["label"] == label]
        per_run = []
        candidates = []
        for row in selected:
            profile = row.get("profile")
            require(isinstance(profile, dict) and profile.get("enabled") is True, "Missing enabled phase profile: " + label)
            categories = profile.get("categories", [])
            require(categories and len({c["name"] for c in categories}) == len(categories), "Missing/duplicate profile category: " + label)
            mapped = {}
            for category in categories:
                require(isinstance(category.get("calls"), int) and category["calls"] >= 0, "Invalid profile call count: " + label)
                for key in ("inclusive_seconds", "exclusive_seconds"):
                    value = category.get(key)
                    require(isinstance(value, (int, float)) and math.isfinite(value) and value >= 0,
                            "Invalid phase timer: " + label + "/" + category["name"])
                mapped[category["name"]] = category
            require("step" in mapped, "Missing step wall timer: " + label)
            per_run.append(mapped)
            candidates.append(profile.get("threshold_candidates"))
        names = list(dict.fromkeys(name for run in per_run for name in run))
        result[label] = {
            "n": len(selected),
            "categories": {name: {
                "available_runs": sum(name in run for run in per_run),
                "calls": stat(run[name]["calls"] for run in per_run if name in run),
                "inclusive_seconds": stat(run[name]["inclusive_seconds"] for run in per_run if name in run),
                "exclusive_seconds": stat(run[name]["exclusive_seconds"] for run in per_run if name in run),
            } for name in names},
            "threshold_candidates_per_run": candidates,
            "timer_scope": "Coordinator inclusive/exclusive wall including worker waits; not total worker CPU. Nested scopes must not be added.",
        }
    return result


def collect(manifest, test_report):
    data = read(manifest)
    require(data.get("status") == "complete", "The measured series is not complete")
    require(data.get("failures") == [], "Driver reports failures")
    definitions = {item["label"]: item for item in data["stage_definitions"]}
    expected_labels = set(DIRECT + PROFILE + GUI)
    require(set(definitions) == expected_labels, "Unexpected stage definition inventory")
    rows = data["runs"]
    counts = Counter(row["label"] for row in rows)
    require(set(counts) == expected_labels, "Missing or unexpected measured labels")
    require(all(counts[label] == (1 if label == "gui_baseline" else 3) for label in expected_labels), "Incorrect repeat counts")
    require(len({row["output"] for row in rows}) == len(rows), "Duplicate run output path")
    checked = {}
    direct_numbers = 0
    direct_numbers_reference_captures = 0
    unavailable_numbers = []
    reference_captures = 0
    for row in rows:
        label = row["label"]
        definition = definitions[label]
        require(row["mode"] == ("gui" if label in GUI else "direct"), "Timer mode differs: " + label)
        require(definition.get("profile_enabled_expected") is (label in PROFILE), "Declared profiling mode differs: " + label)
        # Old GUI executables have no profile.json. Preserve that absence;
        # it is metadata-based non-profile classification, not a runtime flag.
        if label in GUI:
            require(row.get("profile_enabled") in (None, False), "GUI unexpectedly reports profiling enabled: " + label)
        else:
            require(row.get("profile_enabled") is (label in PROFILE), "Runtime profiling mode differs: " + label)
        require(row.get("fixture_sha256") == data["fixture"]["sha256"], "Different fixture bytes: " + label)
        require(row.get("actual_model_seconds") == .05, "Actual interval is not 50ms: " + label)
        vc = row.get("validated_counts", {})
        require(vc.get("steps") == 2000, "Macro-step count differs: " + label)
        for key, expected in (("events", 5771), ("deliveries", 172612)):
            if vc.get(key) is not None:
                require(vc[key] == expected, "Scientific event count differs: " + label + "/" + key)
        require(bool(row.get("exe_sha256")), "Missing measured EXE SHA: " + label)
        comps = row.get("comparisons", {})
        require(comps.get("baseline", {}).get("equal") is True, "Missing successful baseline comparison: " + label)
        for kind, comparison in comps.items():
            if kind == "direct_numbers":
                if comparison.get("equal") is None:
                    require(bool(comparison.get("reason")), "Missing reason for unavailable numeric binary comparison")
                    unavailable_numbers.append({"label": label, "repeat": row["repeat"], "reason": comparison["reason"]})
                    continue
                require(comparison.get("equal") is True, "Negative binary numeric comparison: " + label)
                require(comparison.get("reference_sha256") and comparison.get("reference_sha256") == comparison.get("actual_sha256"), "Contradictory binary numeric comparison")
                direct_numbers += 1
                if comps.get("baseline", {}).get("is_reference"):
                    direct_numbers_reference_captures += 1
                continue
            require(comparison.get("equal") is True, "Negative comparison: " + label + "/" + kind)
            path = resolve(comparison["report_path"])
            if str(path) not in checked:
                checked[str(path)] = check_comparison(path)
            require(checked[str(path)]["valid"], "Invalid saved comparison report: " + str(path))
            if comparison.get("is_reference"):
                reference_captures += 1
        if row["repeat"] > 1:
            require(comps.get("repeat", {}).get("equal") is True, "Missing repeat identity: " + label)
        metric = "wall_seconds" if label in GUI else "solve_seconds"
        require(row.get(metric, 0) > 0, "Measured timer missing: " + label)
        for key in ("peak_working_set_bytes_observed", "max_sampled_private_usage_bytes"):
            require(row.get("memory", {}).get(key) is not None, "Process memory missing: " + label)
    groups = {}
    for label in DIRECT + PROFILE + GUI:
        selected = [r for r in rows if r["label"] == label]
        metric = "wall_seconds" if label in GUI else "solve_seconds"
        groups[label] = {"label": label, "title": TITLES[label], "metric": metric, "profile": label in PROFILE,
                         "time": stat(r[metric] for r in selected),
                         "os_peak_ws_mib": stat(r["memory"]["peak_working_set_bytes_observed"] / 2**20 for r in selected),
                         "sampled_private_mib": stat(r["memory"]["max_sampled_private_usage_bytes"] / 2**20 for r in selected),
                         "recorded_effective_threads": sorted({r.get("effective_threads", r.get("effective_compute_threads")) for r in selected if r.get("effective_threads", r.get("effective_compute_threads")) is not None}),
                         "executable_sha256": sorted({r["exe_sha256"] for r in selected}),
                         "runtime_profile_flags": [r.get("profile_enabled") for r in selected],
                         "definition": definitions[label], "outputs": [r["output"] for r in selected]}
    for label, group in groups.items():
        parent = groups.get(PARENT.get(label))
        if parent:
            require(parent["metric"] == group["metric"] and parent["profile"] == group["profile"], "Incomparable incremental ratio")
        group["comparison_parent"] = parent["label"] if parent else None
        group["ratio"] = parent["time"]["median"] / group["time"]["median"] if parent else None
    tests = check_saved_tests(test_report)
    require(tests["tests"] == 28 and not any(tests[k] for k in ("failures_count", "errors", "skipped", "disabled")), "Fresh tests are not 28/28")
    historical_tests = [check_saved_tests(HERE.parent / "20261019_parallel" / name) for name in
                        ("ctest_preliminary.xml", "ctest_stage4.xml", "ctest_stage5.xml", "ctest_final.xml")]
    validation = read(HERE / "current_validation.json")
    require(validation.get("current_sources_equal_frozen") is True, "Initial production/source verification failed")
    final_validation = read(HERE / "final_validation.json")
    require(final_validation.get("passed") is True, "Final production/source verification failed")
    build = read(HERE.parent / "20261019_parallel/build_manifest.json")["build"]
    return {"generated_utc": datetime.now(timezone.utc).isoformat(), "source": str(manifest), "status": "complete_saved_evidence",
            "fixture": data["fixture"], "host": data.get("host"), "preflight": data.get("preflight"),
            "groups": groups, "phase_profiles": phase_profiles(rows), "comparisons": list(checked.values()), "numeric_binary_comparisons": direct_numbers,
            "numeric_binary_reference_captures": direct_numbers_reference_captures, "archived_final_build": build,
            "unavailable_numeric_binary_comparisons": unavailable_numbers,
            "reference_captures": reference_captures, "tests": tests, "historical_tests": historical_tests,
            "historical_half_second": historical_rows(), "source_validation": validation,
            "final_source_validation": final_validation, "runs": rows,
            "limits": "Only saved reports checked internally; no fresh byte hashing, model execution or tests performed by this generator."}


def table(result, labels):
    lines = ["| Этап | n | Медиана [min;max], с | SD, с | Основа отношения | × | WS MiB¹ | Private MiB¹ |",
             "|---|---:|---|---:|---|---:|---|---|"]
    for label in labels:
        g = result["groups"][label]; t = g["time"]
        memory = lambda key: number(g[key]["median"], 2) + " / " + number(g[key]["max"], 2)
        lines.append(f"| {g['title']} | {t['n']} | {number(t['median'])} [{number(t['min'])};{number(t['max'])}] | {number(t['sample_sd'])} | {g['comparison_parent'] or '—'} | {number(g['ratio'], 2)} | {memory('os_peak_ws_mib')} | {memory('sampled_private_mib')} |")
    return lines


def phase_table(result):
    profiles = result["phase_profiles"]
    previous = profiles[PROFILE[0]]["categories"]
    final = profiles[PROFILE[1]]["categories"]
    names = list(dict.fromkeys(list(previous) + list(final)))
    lines = ["| Фаза | Stage4 calls | Final calls | Stage4 incl / excl, с | Final incl / excl, с |",
             "|---|---:|---:|---:|---:|"]
    for name in names:
        columns = []
        for categories in (previous, final):
            item = categories.get(name)
            if item is None:
                columns.append(("нет данных", "нет данных"))
                continue
            calls = item["calls"]
            count = number(calls["median"], 0)
            if calls["min"] != calls["max"]:
                count += f" [{number(calls['min'], 0)};{number(calls['max'], 0)}]"
            timing = number(item["inclusive_seconds"]["median"], 4) + " / " + number(item["exclusive_seconds"]["median"], 4)
            if item["available_runs"] != 3:
                timing += f" (n={item['available_runs']})"
            columns.append((count, timing))
        lines.append(f"| `{name}` | {columns[0][0]} | {columns[1][0]} | {columns[0][1]} | {columns[1][1]} |")
    return lines


def profile_hotspots(result):
    final = result["phase_profiles"][PROFILE[1]]["categories"]
    ranked = sorted(((name, item["exclusive_seconds"]["median"]) for name, item in final.items()
                     if name != "step" and item["exclusive_seconds"]["median"] > 0),
                    key=lambda pair: pair[1], reverse=True)[:5]
    detail = "; ".join(f"`{name}` — {number(seconds, 3)} с" for name, seconds in ranked)
    return ("В финальном профиле наиболее затратные зарегистрированные exclusive-участки: " + detail
            + ". Это медианы wall одного полного 50-мс расчёта на четырёх потоках. Они выделяют направления дальнейшего анализа, но не измеряют возможное ускорение после новых изменений.")


def render(result):
    groups = result["groups"]
    tests_path = Path(result["tests"]["path"])
    tests_link = tests_path.relative_to(HERE).as_posix() if tests_path.is_relative_to(HERE) else tests_path.as_posix()
    total = groups["s0_baseline"]["time"]["median"] / groups["p5_final8"]["time"]["median"]
    root_ratio = groups["p5_final4"]["ratio"]
    gui_total = groups["gui_baseline"]["time"]["median"] / groups["gui_final8"]["time"]["median"]
    gui_chain = " → ".join(number(groups[label]["time"]["median"]) for label in GUI)
    p1_time = groups["p1_resources4"]["time"]["median"]
    s3_time = groups["s3_runner_direct"]["time"]["median"]
    p1_change = 100 * (p1_time - s3_time) / s3_time
    build = result["archived_final_build"]
    base_time = groups["s0_baseline"]["time"]
    cache_time = groups["s1_caches"]["time"]
    cache_change = 100 * (base_time["median"] - cache_time["median"]) / base_time["median"]
    cache_overlap = max(base_time["min"], cache_time["min"]) <= min(base_time["max"], cache_time["max"])
    cache_direction = "ниже" if cache_change >= 0 else "выше"
    cache_interpretation = (f"Cache-only: медиана {cache_direction} исходной на {number(abs(cache_change), 1)}%. "
        + ("Диапазоны трёх повторов пересекаются; такой небольшой выигрыш — слабое свидетельство, его устойчивость этой серией не установлена. "
           if cache_overlap and 0 <= cache_change < 5 else
           "Диапазоны трёх повторов пересекаются; уверенно отделить эффект от наблюдаемого разброса по этой серии нельзя. "
           if cache_overlap else
           "Диапазоны этих трёх повторов не пересекаются, однако n=3 и фиксированный порядок не доказывают устойчивость на других запусках. ")
        + f"Следующий переход к компактному ядру даёт отношение медиан {number(groups['s2_compact']['ratio'], 2)}× относительно версии с кэшами. Общий результат нельзя целиком приписывать кэшированию.")
    lines = ["# Network Studio: повторная проверка этапов оптимизации", "",
        f"**Завершены {len(result['runs'])}/{len(result['runs'])} запусков с успешными численными сравнениями и 28/28 текущих тестов.** Новая общая серия охватывает 50 модельных мс. Direct solve: {number(groups['s0_baseline']['time']['median'])} → {number(groups['p5_final8']['time']['median'])} с, отношение медиан **{number(total, 2)}×**.", "",
        f"Native GUI: **{gui_chain} с** для исходной версии, компактного ядра, runner и final8 соответственно. Совокупное отношение — **{number(gui_total, 2)}×**; исходный GUI измерен один раз, остальные версии — трижды. Wall охватывает цикл расчёта до подтверждённого завершения; setup, экспорт и checkpoint находятся вне этого таймера и входят в отдельный process_seconds. Это не оценка одного участка solver. 150 модельных секунд в этой серии не рассчитывались.", "",
        "## Условия и происхождение", "",
        "500 нейронов Ижикевича (400 E / 100 I), 15 013 TM-синапсов, мультипликативная STDP, шум σ = 5 с интервалом 1 мс, seed = 706, dt = 25 мкс, запись каждые 2 мс. Это производная сохранённого примера, не точная идентификация первоначальной сети примерно из 15 041 связи. План лабораторных стадий не активируется. Во всех новых запусках использован один файл с одинаковым SHA; 2000 фактических макрошагов, 5771 спайк, 172 612 доставок.", "",
        "Площадка прежняя: Intel Core i9-11900KF, Windows 11, 16 логических CPU, около 39,9 GiB RAM. Процессы выполнялись последовательно; температура и сторонняя нагрузка не контролировались. Наблюдаемый разброс не является доверительным интервалом. SHA EXE, команды, параметры и сведения о происхождении сохранены в [measured/runs.json](measured/runs.json), [машинной сводке](report_data.json).", "",
        f"Финальный архив: {build['compiler']}, x64, Release, C++17, `/O2 /Ob2 /DNDEBUG /MT`; fast-math (`/fp:fast`) не включён. Полные флаги и build ID приведены в [архивных метаданных сборки](../20261019_parallel/build_manifest.json). Профильные версии явно отделены от непрофильных.", "",
        "## НОВЫЕ direct-измерения", "",
        "Release без profiling; только solve time. По три повтора. × — медиана явно указанного сопоставимого эталона / текущая медиана. Переход S3→P1 включает переход с одного на четыре вычисляющих потока; p5_final4→p5_final8 — масштабирование одной реализации.", ""]
    lines += table(result, DIRECT)
    lines += ["", cache_interpretation, "",
        f"Отдельный ранний этап P1 с параллельным обновлением TM-ресурсов дал {number(p1_time)} с против {number(s3_time)} с у S3: на {number(p1_change, 1)}% медленнее. Ускорение возникло после последующих этапов, а не от P1 самого по себе. Накладные расходы распределения работы — возможное объяснение; их причинный вклад этим сравнением не измерен.", "",
        f"**control_no_roots4 — новая абляция из замороженной финальной реализации**, где только поиск корней выполняется последовательно. Это не восстановленный исторический P4. Переход P3→control объединяет остальные промежуточные правки и не измеряет отдельный вклад P4: историческая сборка P4 с выключенным profiling не сохранена. Поэтому отношение к предыдущей строке P3 не приводится; только control→final4 проверяет локальное изменение поиска корней при одинаковых четырёх потоках ({number(root_ratio, 2)}×). Производственная сборка не подменяется контрольной. Входная проверка [current_validation.json](current_validation.json) связала 705 текущих файлов с финальным снимком; повторная проверка после измерений сохранена в [final_validation.json](final_validation.json).", "",
        "## НОВЫЕ профильные измерения", "",
        "Оба сохранённых profile EXE запускаются на четырёх потоках. Их времена не смешиваются с предыдущей таблицей. Сравниваются две исторически разные реализации; отсутствие отдельного полного исходного снимка stage4 ограничивает приписывание всей разницы одной оптимизации.", ""]
    lines += table(result, PROFILE)
    lines += ["", "Ниже медианы фаз из трёх профильных запусков каждой версии. Calls — медиана [min;max], если счётчик менялся. Полные min/max/SD каждого таймера и исходные счётчики кандидатов сохранены в report_data.json; отсутствие категории обозначалось бы явно.", ""]
    lines += phase_table(result)
    lines += ["", "Inclusive-таймеры вложены и перекрываются: например, step содержит остальные фазы, trial содержит RHS, а thresholds/event_jumps имеют вложенные измерения. Их нельзя складывать как независимые затраты. Сумма отдельных медиан также не равна медиане общего времени. Exclusive исключает зарегистрированные вложенные scope только на измеряемом потоке.", "",
        "Счётчики профиля содержат inclusive/exclusive wall на координаторе и ожидании helpers, не сумму CPU-времени всех workers. В финальном пути rhs_synapses включает производные нейронов; нулевой rhs_neurons не означает отсутствия вычисления нейронов. Счётчик threshold_sample может охватывать только вызовы координатора. Разность числа зарегистрированных вызовов сама по себе не доказывает уменьшения общего объёма математической работы.", "",
        profile_hotspots(result), "",
        "## НОВЫЕ native GUI-измерения", "",
        "Wall timer цикла расчёта до подтверждённого завершения, включая GUI draw/render/Present; setup, экспорт и checkpoint исключены. Отдельный process_seconds включает весь процесс. GUI wall нельзя делить на direct solve и называть это ускорением одного участка. Исходный GUI измерен один раз (SD не определено), остальные — трижды.", ""]
    lines += table(result, GUI)
    lines += ["", "Скрытое настоящее окно Win32/DX11 с Present(1) проходит native GUI loop. Это не ручная сессия видимого GUI и не GPU timer. Частота публикации, renderer, Present/occlusion и CPU draw/render находятся в исходных строках manifest. У старого GUI нет profile.json: non-profile классификация опирается на метаданные сборки, отсутствие runtime-флага сохранено явно. Отсутствующие научные counts GUI не заменены числом переданных delta-пакетов: соответствие reference подтверждают полные CSV и checkpoint.", "",
        "¹ Память: медиана / максимум среди запусков, весь процесс включая setup/export/checkpoint. OS PeakWorkingSet — максимум до последнего успешного опроса; sampled PrivateUsage может пропустить краткие пики. Это не точный lifetime private peak и не объём исключительно solver.", "",
        "## Численная идентичность и тесты", "",
        f"Проверены {len(result['comparisons'])} сохранённых отчётов сравнения, **включая {result['reference_captures']} самосравнение эталона**, и {result['numeric_binary_comparisons']} записей binary-numbers comparison, **включая {result['numeric_binary_reference_captures']} самосравнение эталона**. Это не количество независимых экспериментов: повторные проверки и проверки относительно общего reference используют пересекающиеся данные. В остальных отчётах сравниваются разные сохранённые запуски. Число недоступных binary-numbers проверок — {len(result['unavailable_numeric_binary_comparisons'])}; пропуски не считаются успешным совпадением.", "",
        "Все 10 CSV совпали побайтово; для одной сборки checkpoint совпадает полностью, между сборками разрешены только build ID и зависимая checksum. Исходные суммы, схема, алгоритм, config и остальные bytes проверяются comparator. Это офлайн-сравнение, без загрузки checkpoint в несовместимую сборку или обхода проверки совместимости. Генератор отчёта повторно не хеширует данные и не запускает solver.", "",
        f"Свежий [CTest](<{tests_link}>): **28/28**, без пропусков; это текущая финальная реализация. Для P1–P3 сохранены benchmark EXE и новые численные сравнения, но отсутствуют полные исторические unit-test EXE и отдельные исходные снимки. Нельзя выдавать запуск текущих тестов за полную повторную проверку каждого исторического этапа.", "",
        "Ранние результаты сохранены как есть: preliminary 3/4 — `invalid delivery timestamp or disabled edge`; stage4 25/27 — `lost or extra delivery` и GUI `Izhikevich parameters are missing`; stage5 8/8; прежний final 28/28. Неудачи не объявлены успешными задним числом. Лог сам по себе не доказывает, что причина только в тесте или только в модели. Точные исходные записи включены в report_data.json.", "",
        "## СТАРЫЕ измерения на 0,5 модельной секунды", "",
        "Это результаты предыдущих серий, не новые запуски общего протокола 50 мс. Они имеют другую длительность, дату внутри сеанса и в строке runner — другой таймер. Из них не вычисляются отношения между новыми этапами.", "",
        "| Историческая серия | Таймер | n | Медиана [min;max], с | SD, с |", "|---|---|---:|---|---:|"]
    for item in result["historical_half_second"]:
        t = item["time"]
        lines.append(f"| [{item['name']}]({item['path']}) | {item['metric']} | {t['n']} | {number(t['median'])} [{number(t['min'])};{number(t['max'])}] | {number(t['sample_sd'])} |")
    lines += ["", "## Границы выводов и воспроизведение", "",
        "Для этой сети из 500 нейронов на измеренном CPU рекомендуется Auto (фактически 8 потоков) либо явные 8 потоков. Для малых сетей разумны 1 поток и предусмотренный автоматический fallback; настройку для другой сети нужно измерять отдельно. Новый протокол сравнивал final на 4 и 8 потоках; измерения 1/2/4/8/Auto на 0,5 с приведены выше как исторические.", "",
        "150 модельных секунд этой сети не рассчитаны. Меняющиеся спайковая активность, STDP-веса, локализация событий, история и скользящие журналы могут нарушить линейную экстраполяцию. Малая сеть/другой CPU/ARM могут иметь иной оптимум; Auto — аппаратно зависимая эвристика. ThreadSanitizer и длительная ручная GUI-сессия этой серией не подтверждаются.", "",
        "Промежуточные EXE идентифицированы записанными SHA и checkpoint build ID. Для stage1–3 нет полного исторического исходного архива; для старого непрофилируемого stage4 нет отдельного EXE. Новая абляция и профильный контроль заполняют разные экспериментальные вопросы и не заменяют эти отсутствующие артефакты. Финальный полный [снимок исходников](../20261019_parallel/source_snapshot.zip) и [manifest](../20261019_parallel/build_manifest.json) сохранены отдельно.", "",
        "Методика и ограничения: [NOTES_RU.md](NOTES_RU.md). Driver печатает план без --execute; выполняйте только последовательные запуски в новый каталог. Для генерации отчёта после завершения измерений:", "", "```powershell",
        "python -B performance/20261019_stage_review/run_stages.py --help",
        "python -B performance/20261019_stage_review/build_report.py --write", "```", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=HERE / "measured/runs.json")
    parser.add_argument("--tests", type=Path, default=HERE / "ctest.xml", help="Saved fresh CTest JUnit report; must be 28/28")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    require(not (HERE.parent / ".benchmark.lock").exists(), "Measurement process is active; wait before report generation")
    result = collect(args.manifest.resolve(), args.tests.resolve())
    report = render(result)
    if args.write:
        (HERE / "report_data.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (HERE / "REPORT_RU.md").write_text(report, encoding="utf-8")
        print(f"Wrote REPORT_RU.md ({len(report.splitlines())} lines), report_data.json; no model/benchmark/test was run")
    else:
        print(report)


if __name__ == "__main__":
    main()
