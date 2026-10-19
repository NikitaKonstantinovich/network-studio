"""Render the concise final report from COMPLETED saved evidence, without solving.

Run only after the final measurement processes have stopped. Reads small JSON
and XML reports; does not hash exports/EXEs or invoke any executable. Required
series must be complete and their saved numerical-comparison reports valid.
Writes REPORT_RU.md only with --write; default prints the report to stdout.
"""
import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

import summarize as summary_tools

HERE = Path(__file__).resolve().parent
SERIES = ("calibration_50ms", "crossover_v2", "final_100ms", "final_500ms", "integration_final", "profile_final")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def f(value, digits=3):
    return f"{value:.{digits}f}"


def spaced(value):
    return f"{value:,}".replace(",", " ")


def load():
    for name in SERIES:
        manifest = HERE / name / ("integration.json" if name == "integration_final" else "runs.json")
        require(manifest.is_file(), "Missing completed manifest: " + str(manifest))
    rows, comparisons, issues, sources = summary_tools.collect([HERE / name for name in SERIES])
    integration = summary_tools.collect_integration([HERE / "integration_final"], comparisons, rows, issues)
    groups = summary_tools.aggregate(rows, issues)
    require(not issues, "Saved evidence is incomplete/inconsistent: " + json.dumps(issues, ensure_ascii=False))
    require(comparisons and all(c["valid"] for c in comparisons), "Not every saved numerical comparison passed")
    index = {(g["series"], g["workload"], g["label"]): g for g in groups}

    def group(series, label, duration, count, workload="burst"):
        key = (series, workload, label)
        require(key in index, "Missing completed group: " + str(key))
        g = index[key]
        require(g["time_seconds"]["n"] == count and g["model_seconds"] == duration,
                "Unexpected repeat count/duration: " + str(key))
        require(len(g["steps"]) == 1 and len(g["events"]) == 1 and len(g["deliveries"]) == 1,
                "Inconsistent or missing actual counts: " + str(key))
        if label != "frozen":
            expected = 8 if label == "threads_0" else int(label.removeprefix("threads_"))
            require(g["effective_threads"] == [expected], "Unexpected effective thread count: " + str(key))
        return g

    calibration = [group("calibration_50ms", "threads_" + str(t), .05, 3, w)
                   for w in ("quiet", "burst") for t in (1, 2, 4, 8)]
    full = [group("final_500ms", label, .5, 1) for label in ("frozen", "threads_1", "threads_2", "threads_4", "threads_8", "threads_0")]
    intermediate = [group("final_100ms", label, .1, 1) for label in ("frozen", "threads_1", "threads_8")]
    profile_group = group("profile_final", "threads_8", .05, 1)
    profile = read(HERE / "profile_final/burst/threads_8/run_1/profile.json")
    require(profile.get("enabled") is True, "Final profile counters disabled")
    require(len(integration) == 1, "Expected one final integration series")
    native = {row["name"]: row for row in integration[0]["runs"]}
    expected_names = {"direct_1", "gui_1", "gui_auto", "runner_hz1", "runner_hz10", "runner_hz60", "cli_prefix_2", "cli_resume_8"}
    require(set(native) == expected_names, "Integration must contain all eight completed processes")
    for name in ("gui_1", "gui_auto", "runner_hz1", "runner_hz10", "runner_hz60"):
        row = native[name]
        require(row["metric"] == "wall_seconds" and row["observed_model_final_seconds"] == .05,
                "Unexpected integration timer/final duration: " + name)
        require(row["effective_threads"] == (1 if name == "gui_1" else 8), "Unexpected final Auto: " + name)
    for name in ("gui_1", "gui_auto"):
        require(native[name]["timing"].get("hidden") is True, "GUI scope is not the documented hidden native window")
    for name, start in (("cli_prefix_2", 0), ("cli_resume_8", .025)):
        require(native[name]["start_model_seconds"] == start and native[name]["integrated_seconds"] == .025,
                "CLI split no longer matches 25+25ms")
    final_exe = read(HERE / "final_500ms/runs.json")["binaries"]["new"]["sha256"]
    require(read(HERE / "final_100ms/runs.json")["binaries"]["new"]["sha256"] == final_exe,
            "Final direct measurements used different EXEs")
    require(native["direct_1"]["exe_sha256"] == final_exe, "Integration direct EXE differs from final measurement")
    tests = ET.parse(HERE / "ctest_final.xml").getroot()
    require(int(tests.get("tests", -1)) == 28 and all(int(tests.get(k, 0)) == 0 for k in ("failures", "errors", "skipped", "disabled")),
            "Final CTest evidence is not 28/28 with no skipped tests")
    parallel_text = "\n".join("".join(case.itertext()) for case in tests.findall("testcase") if case.get("name") == "parallel_solver_invariance")
    match = re.search(r"PASS: (\d+) parallel checks; (\d+) exact macro states", parallel_text)
    require(match is not None, "Missing parallel invariance counters")
    checks, states = map(int, match.groups())
    require((checks, states) == (13839964, 3393), "Parallel validation changed; review report scope")
    require("automatic stages and shared stimuli" in parallel_text, "Missing scheduled-stage test evidence")
    rejection = read(HERE / "old_checkpoint_rejection.json")
    require(rejection.get("rejected_as_expected") is True and rejection.get("returncode") != 0
            and rejection.get("integrated_seconds") == 0 and "incompatible solver build" in rejection.get("output", ""),
            "Old-build checkpoint was not explicitly rejected before integration")
    return {"calibration": calibration, "full": full, "intermediate": intermediate, "profile": profile,
            "profile_group": profile_group, "native": native, "comparisons": comparisons,
            "environment": read(HERE / "environment.json"), "final_exe_sha256": final_exe,
            "checks": checks, "states": states, "sources": sources}


def render(data):
    full = {g["label"]: g for g in data["full"]}
    baseline = full["threads_1"]["time_seconds"]["median"]
    auto = full["threads_0"]["time_seconds"]["median"]
    events, deliveries = full["threads_0"]["events"][0], full["threads_0"]["deliveries"][0]
    env = data["environment"]
    lines = ["# Network Studio: параллельный решатель — измерения и проверка", "",
        f"На проверенной сети Auto выбрал 8 вычисляющих потоков. Расчёт 0,5 модельной секунды занял {f(auto)} с вместо {f(baseline)} с на одном потоке той же сборки: ускорение {f(baseline / auto, 2)}×. Это одиночный полный прогон, не оценка разброса и не измерение 150-секундного опыта.", "",
        "## Сеть и условия", "",
        "500 нейронов Ижикевича (400 E / 100 I), **15 013** TM-синапсов, мультипликативная STDP, шум σ=5, seed 706, шаг 25 мкс, шум обновляется через 1 мс, запись через 2 мс. Это производная сохранённого опыта, а не точная идентификация первоначальной сети с примерно 15 041 связью. Quiet отличается только отсутствием глобального шума и служебными названием/длительностью. Лабораторные результаты не перезаписывались; [происхождение fixture](provenance.json).", "",
        f"{env['cpu']['ProcessorNameString']}; {env['logical_processors']} логических CPU, {f(env['total_physical_bytes'] / 2**30, 2)} GiB RAM; {env['platform']}. Последовательные процессы, Release без profiling/fast-math; фиксированный порядок настроек. Температура и фоновая загрузка не контролировались. SHA прямого final EXE: `{data['final_exe_sha256']}`.", "",
        "## Изменения", "",
        "Фиксированный пул выполняет независимые TM-кинетики, производные/токи нейронов, STDP по синапсам, физические ограничения и независимый поиск корней. Порядок суммирования входящих токов, обработка событий/доставок, уравнения, шаг, допуски и точность сохранены. Смена числа потоков проходит на границе шага; полный checkpoint не включает политику исполнения. Подробности: [архитектура](ARCHITECTURE_RU.md).", "",
        "## Калибровка на 50 мс", "",
        "[Calibration](calibration_50ms/runs.json): n=3 для каждой строки, 2000 реальных макрошагов. Время — median [min;max], SD выборочное, только solve. Quiet: 0 спайков; burst: 5771 спайк и 172 612 доставок. Это более ранняя калибровочная сборка; Auto выбирался после неё.", "",
        "| Нагрузка | Потоки | Время, с | SD, с | × new1 |", "|---|---:|---|---:|---:|"]
    for g in data["calibration"]:
        t = g["time_seconds"]
        lines.append(f"| {g['workload']} | {g['effective_threads'][0]} | {f(t['median'])} [{f(t['min'])}; {f(t['max'])}] | {f(t['sample_sd'])} | {f(g['speedup_vs_new1'], 2)} |")
    lines += ["", "## Полный интервал 0,5 с и контроль 0,1 с", "",
        f"[Final500](final_500ms/runs.json): **n=1**, 20 000 шагов, {spaced(events)} спайков, {spaced(deliveries)} доставок. SD не определено. Ускорения new1/frozen вычислены относительно разных эталонов при одинаковой длительности и fixture.", "",
        "| Настройка→факт | Solve, с | × new1 | × frozen | OS peak WS, MiB | Sampled private, MiB |",
        "|---|---:|---:|---:|---:|---:|"]
    for g in data["full"]:
        label = "frozen" if g["label"] == "frozen" else ("Auto" if g["label"] == "threads_0" else g["label"].removeprefix("threads_")) + "→" + str(g["effective_threads"][0])
        require(g["peak_working_set_mib"] and g["max_sampled_private_mib"], "Final process memory missing")
        lines.append(f"| {label} | {f(g['time_seconds']['median'])} | {f(g['speedup_vs_new1'], 2)} | {f(g['speedup_vs_frozen'], 2)} | {f(g['peak_working_set_mib']['max'], 2)} | {f(g['max_sampled_private_mib']['max'], 2)} |")
    lines += ["", "Память измерена для всего процесса, включая setup/export/checkpoint: PeakWorkingSet — максимум ОС до последнего успешного опроса, PrivateUsage — максимум выборки 100 мс, короткие пики могут быть пропущены.", "",
        "[Final100](final_100ms/runs.json), n=1, 4000 шагов (контроль масштаба, не оценка разброса):", "",
        "| Режим | Solve на 0,1 с, с | × new1 |", "|---|---:|---:|"]
    for g in data["intermediate"]:
        label = "frozen" if g["label"] == "frozen" else g["label"].removeprefix("threads_")
        lines.append(f"| {label} | {f(g['time_seconds']['median'])} | {f(g['speedup_vs_new1'], 2)} |")
    lines += ["", "Во время серии 100 мс было краткое стороннее чтение файлов (~0,85 с); возможный I/O-фон отдельно ограничивает эту проверку. Основная 50-миллисекундная калибровка и final500 этим чтением не затронуты. Начальный smoke на 5 мс имел только 6 спайков/161 доставку и не представлял плотную нагрузку.", "",
        "## GUI, публикация снимков и resume", "",
        "[Integration](integration_final/integration.json): по одному запуску, 50 мс; ниже **wall**, исключающий setup/export, а не solve timer CLI. Каждый результат совпал с direct serial побайтово по десяти CSV и полному checkpoint.", "",
        "| Native GUI / runner | Потоки→факт | Wall, с | × GUI1 |", "|---|---|---:|---:|"]
    for name in ("gui_1", "gui_auto", "runner_hz1", "runner_hz10", "runner_hz60"):
        row = data["native"][name]
        requested = "Auto" if row["requested_threads"] == 0 else str(row["requested_threads"])
        ratio = f(row["gui_speedup_vs_gui1"], 2) if row["gui_speedup_vs_gui1"] is not None else "—"
        lines.append(f"| {name} | {requested}→{row['effective_threads']} | {f(row['measured_seconds'])} | {ratio} |")
    gui = data["native"]["gui_auto"]["timing"]
    lines += ["", f"GUI — скрытое настоящее окно Win32/DX11, Present(1), renderer={gui.get('renderer', 'не записан')}; Auto: Present ok={gui.get('present_ok', '—')}, occluded={gui.get('present_occluded', '—')}. Это не ручная сессия видимого GUI и не GPU timer. Runner 10/1/60 Гц проверяет независимость чисел от публикации; delta-пакеты не считаются уникальными событиями.", "",
        "Обычный CLI прошёл 25 мс на 2 потоках, сохранил checkpoint и штатно продолжил следующие 25 мс на 8 потоках. Финальный checkpoint и все 10 CSV совпали с непрерывным serial. Таймеры этих двух CLI-процессов включают загрузку/экспорт и не участвуют в таблицах solve/wall.", "",
        "## Корректность и оставшиеся затраты", "",
        f"[CTest final](ctest_final.xml): **28/28**, без пропусков. Параллельная матрица: {spaced(data['states'])} побайтных макросостояний, {spaced(data['checks'])} проверок, 1/2/4/8/Auto, повторы, cross-count resume, fallback, lifecycle и отказ на nonfinite. Отдельный тест выполняет 3 стадии по 1 мс (3 мс суммарно), общие стимулы, заморозку обучения и очистку STDP на границе; это не 150 с лабораторного протокола.", "",
        "Матрица использует 64 нейрона / 2048 TM, по 3 модельных мс: общий и индивидуальный шум, группу из 64 одновременных спайков, задержки 0 / 0,375 / 1,125 мс, arrival/emission, аддитивную/мультипликативную/триплетную STDP и замороженные веса с продолжающими затухать следами. Проверены все 16 переходов checkpoint между 1/2/4/8 потоками, повторные паузы/продолжения и освобождение всех помощников после shutdown.", "",
        "[Нативное закрытие](native_close_parallel.json): 128 нейронов / 4096 TM, 4 потока, WM_CLOSE во время расчёта на 4,825 модельной мс; завершение и освобождение ресурсов заняло 15,60 мс. [Последовательный контроль](native_close_serial.json) и тесты жизненного цикла сохранены отдельно. Эти задержки закрытия относятся к короткому опыту, не являются гарантией верхней границы для любой сети.", "",
        f"У {len(data['comparisons'])} сохранённых comparison.json выбранных серий проверены согласованные успешные флаги и хеши отчётов. Все 10 CSV обязаны совпасть побайтово; checkpoint той же сборки — целиком. Между сборками исключены только build ID и зависимая checksum, с проверкой исходных сумм, схемы, алгоритма, config и всех остальных bytes. Генератор отчёта не перечитывает численные файлы. [Старый checkpoint](old_checkpoint_rejection.json) штатно отклонён до интегрирования: compatibility не обходилась.", "",
        f"[Профиль final8](profile_final/burst/threads_8/run_1/profile.json): 50 мс, solve={f(data['profile_group']['time_seconds']['median'])} с со включёнными счётчиками. Это диагностика, не основной throughput. Inclusive включает вложенные фазы, exclusive их вычитает; ниже wall на координаторе/ожидании пула, не сумма CPU workers.", "",
        "| Категория | Inclusive, с | Exclusive, с |", "|---|---:|---:|"]
    categories = {c["name"]: c for c in data["profile"]["categories"]}
    for name in ("trial", "rhs_synapses", "bounds", "thresholds", "exact_synapses", "stdp"):
        c = categories[name]
        lines.append(f"| {name} | {f(c['inclusive_seconds'])} | {f(c['exclusive_seconds'])} |")
    lines += ["", "В оптимизированном пути `rhs_synapses` включает также производные нейронов; нулевой отдельный rhs_neurons не означает отсутствие этой работы. Последовательные события, шум, журналы, координация trial/root и синхронизация ограничивают дальнейшее масштабирование; inclusive-столбцы нельзя суммировать как независимые затраты.", "",
        "## Auto и границы вывода", "",
        "Для только-Izh/TM без астроцитов: меньше 2048 синапсов → 1, 2048–8191 → предел 4, от 8192 → предел 8; выбирается наибольшее 1/2/4/8 не выше hardware_concurrency, неизвестное значение → 1. Явные настройки сохранены. В малых crossover (36 запусков по 5 мс) преимущество 8 над 4 невелико относительно разброса, поэтому выбрана сдержанная политика 4; для основной сети 8 измеренно быстрее. Это аппаратно зависимая эвристика, не доказанный оптимум для ARM/других CPU.", "",
        f"**150 модельных секунд этой сети не рассчитаны.** Формальная линейная оценка от 0,5 с: `solve × 300 / 3600` часов; serial ≈ {f(baseline * 300 / 3600, 2)} ч, Auto ≈ {f(auto * 300 / 3600, 2)} ч. В benchmark сохранённый план стадий не активировался. Меняющиеся активность, STDP-веса, плотность событий, история и скользящие журналы могут сделать такую экстраполяцию неверной.", "",
        "Ручная проверка длительной видимой GUI-сессии и ThreadSanitizer не проводились. Побайтные тесты и lifecycle повышают доверие к проверенным путям, но не доказывают отсутствие всех гонок и не заменяют повтор на другом оборудовании/другой сети.", "",
        "## Воспроизведение и архив", "",
        "Команды из корня configurator; выходные каталоги должны быть новыми, выполняйте строго последовательно. Большие fixture готовятся prepare.py/crossover.py --prepare; все исходные команды и SHA EXE находятся в runs.json/integration.json.", "", "```powershell",
        "./performance/build.ps1 -Build build/parallel -Targets all",
        "python -B performance/20261019_parallel/prepare.py",
        "python -B performance/20261019_parallel/crossover.py --prepare",
        "python -B performance/20261019_parallel/measure.py --phase diagnostic --threads 1 2 4 8 --repeat 3 --out performance/repro_cal50 --execute",
        "python -B performance/20261019_parallel/crossover.py --duration .005 --repeat 3 --out performance/repro_cross --execute",
        "python -B performance/20261019_parallel/measure.py --phase intermediate --workloads burst --threads 1 8 --out performance/repro_100 --execute",
        "python -B performance/20261019_parallel/measure.py --phase full --workloads burst --threads 1 2 4 8 0 --out performance/repro_500 --execute",
        "python -B performance/20261019_parallel/integration.py --out performance/repro_integration --threads 0 --execute",
        "./performance/build.ps1 -Build build/parallel-profile -Profile -Targets network_studio_benchmark",
        "python -B performance/20261019_parallel/measure.py --phase diagnostic --workloads burst --threads 1 8 --new-dir build/parallel-profile/Release --out performance/repro_profile --execute",
        "ctest --test-dir build/parallel --output-on-failure --parallel 1",
        "python -B performance/20261019_parallel/summarize.py --write-report",
        "python -B performance/20261019_parallel/make_report.py --write", "```", "",
        "Полная методика/сборка/профиль: [README](README_RU.md), подробные измерения: [приложение](MEASUREMENTS_RU.md). Пути пакета воспроизведения: [parallel-release](parallel-release/), [source_snapshot.zip](source_snapshot.zip), [build_manifest.json](build_manifest.json). Завершение архива подтверждает только manifest, который записывается последним; он связывает исходники, toolchain, flags, checkpoint build ID и EXE. Ссылка сама по себе не доказывает, что архив уже создан.", ""]
    require(len(lines) <= 150, f"Report is too long: {len(lines)} lines")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="Write REPORT_RU.md after all required saved series pass checks")
    args = parser.parse_args()
    require(not (HERE.parent / ".benchmark.lock").exists(), "A measurement process is active; wait before generating the final report")
    report = render(load())
    if args.write:
        target = HERE / "REPORT_RU.md"
        target.write_text(report, encoding="utf-8")
        print(f"Wrote {target}; {len(report.splitlines())} lines. No solver or numerical comparator executed.")
    else:
        print(report)


if __name__ == "__main__":
    main()
