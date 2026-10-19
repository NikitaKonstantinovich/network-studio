"""Assemble the report from completed measurements and logs, without running simulations."""
import difflib
import json
from pathlib import Path
import re
from measure import HERE, ROOT, COUNTS, sha, write


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


measure = load(HERE / "measurements_final/runs.json")
calibration = load(HERE / "measurements/summary.json")
calibration_build = load(HERE / "calibration_build_test_time.json")
build = load(HERE / "final_build_test_time.json")
targeted = load(HERE / "targeted_time.json")
compat = load(HERE / "compatibility_final/report.json")
assert build["exit_code"] == targeted["exit_code"] == 0
assert len(measure["runs"]) == len(COUNTS) * 3 * measure["repeats"]
assert all(r["exact_match"] for r in measure["runs"])
log = (HERE / "final_build_test.log").read_text(encoding="utf-8-sig", errors="replace")
ctest = re.search(r"100% tests passed, 0 tests failed out of (\d+)", log)
assert ctest
ctest_seconds = float(re.search(r"Total Test time \(real\) =\s*([\d.]+) sec", log)[1])
test_log = (HERE / "final_ctest.log").read_text(encoding="utf-8-sig", errors="replace")
matrix = re.search(r"PASS: (\d+) parallel checks; (\d+) exact macro states", test_log)
assert matrix
manifest = {"branch": "perf/thread-modes-6-12-16-20261019", "executables": {},
            "checkpoint_build_header": (ROOT / "build/native/checkpoint_build.h").read_text().strip(),
            "ctest_count": int(ctest[1]), "ctest_seconds": ctest_seconds,
            "final_build_test_smoke_seconds": build["seconds"], "targeted_seconds": targeted["seconds"],
            "measurement_seconds": measure["elapsed_seconds"], "compatibility_seconds": compat["elapsed_seconds"],
            "calibration_measurement_seconds": calibration["elapsed_seconds"],
            "calibration_build_test_smoke_seconds": calibration_build["seconds"],
            "parallel_checks": int(matrix[1]), "exact_macro_states": int(matrix[2]),
            "source_hashes": {}, "numerical_changes": "No arithmetic changes: mode validation, per-worker capacities and Auto target 6 for 4096..8191 synapses; existing arithmetic and event ordering retained."}
for name in ("network_studio.exe", "network_studio_cli.exe", "network_studio_benchmark.exe"):
    path = ROOT / "build/Release" / name
    manifest["executables"][name] = {"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size}
assert manifest["executables"]["network_studio_benchmark.exe"]["sha256"] == measure["exe_sha256"]
patch = []
for original in sorted((HERE / "baseline_sources").rglob("*")):
    if not original.is_file():
        continue
    relative = original.relative_to(HERE / "baseline_sources")
    current = ROOT / relative
    before, after = original.read_text(encoding="utf-8-sig"), current.read_text(encoding="utf-8-sig")
    manifest["source_hashes"][relative.as_posix()] = sha(current)
    patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                    fromfile="before/" + relative.as_posix(), tofile="after/" + relative.as_posix()))
manifest["source_hashes"]["numerics/compute_threads.h"] = sha(ROOT / "numerics/compute_threads.h")
(HERE / "task_changes.patch").write_text("".join(patch), encoding="utf-8")
write(HERE / "manifest.json", manifest)

lines = ["# Вычислительные потоки 6 / 12 / 16", "",
    "Дата работы: 19–20.10.2026. Ветка `perf/thread-modes-6-12-16-20261019`. Исходные незакоммиченные изменения сохранены: список и patch в `baseline_status.txt` / `baseline_tracked.patch`, копии затронутых файлов в `baseline_sources/`. Только изменения этой задачи относительно исходного рабочего дерева — `task_changes.patch`; новые файлы хранятся рядом. Предыдущие отчёты не изменены.", "",
    "## Реализация", "",
    "Общий набор `1/2/4/6/8/12/16/Авто` задан в `numerics/compute_threads.h`: меню GUI, ASCII/Unicode разбор аргументов CLI/GUI, справка, проверка Simulator и максимальная ёмкость пула используют его. Расширены слоты пула, STDP-кэши, scratch локализации порогов и массивы редукций физических ограничений/ошибки до 16. `SimulationRunner` применяет прежнюю типизированную команду между полностью завершёнными шагами; дополнительных ограничений там не было.", "",
    "Число включает вычисляющий координатор, поэтому 16 = координатор + 15 помощников. GUI показывает запрошенный режим и фактическое число из подтверждённого снимка; интерфейсный поток не считается. При смене создаётся новый пул, старые помощники join-ятся до подтверждения. Блоки `[n*i/count, n*(i+1)/count)` работают для неполных и пустых диапазонов; степени двойки не требуются. Число потоков не сериализуется в научной конфигурации/checkpoint.", "",
    "Уравнения, dt, допуски, double, порядок суммирования токов и событий не менялись. Release MSVC /O2 без fast-math; profiling OFF. Смена режимов не меняет RNG/семантику шума. Заголовок политики включён в строгую сигнатуру совместимости сборки checkpoint; проверки совместимости не отключались.", "",
    "## Проверки", "",
    f"Штатный `build.ps1 -Test -Smoke`: **{ctest[1]}/{ctest[1]} CTest**, затем штатный Win32/DX11 smoke; CTest {ctest_seconds:.2f} с, весь build+tests+smoke {build['seconds']:.2f} с. Перед ним целевые 6/6 тестов заняли {targeted['seconds']:.2f} с. Логи: [финальная сборка](final_build_test.log), [CTest](final_ctest.log), [целевые проверки](targeted.log). Тесты вне измерений могут исполняться параллельно (CTEST_PARALLEL_LEVEL=4), native/parallel-тесты RUN_SERIAL.", "",
    f"Матрица: **{int(matrix[1]):,} проверок, {int(matrix[2]):,} точных макросостояний**, по два прогона каждого из 1/2/4/6/8/12/16 для трёх сценариев по 3 модельных мс. Сеть 64 нейрона / 2048 TM: шум, 64 одновременных спайка, задержки 0/0,375/1,125 мс, arrival/emission, три правила STDP, замороженные веса/живые следы. Сравниваются double побайтно и полные файлы checkpoint со всеми метаданными. Все **49** переходов checkpoint между ручными режимами; обычный совместимый loader, без подмены ID/checksum.", "",
    "Пул: размеры 0/1/5/11/17/2049, атомарные счётчики посещений (ровно один раз), исключения координатора/последнего рабочего, повторный запуск и пробуждение. Проверены реально созданные Windows-потоки и отсутствие прежних помощников после 6→12→16→8→4→2→1. Runner меняет режим на ходу, подтверждает паузу, сохраняет состояние/предпочтение при reset/restore, завершает работу и join всех помощников. Малые сети, HH/астроциты и смешанные синапсы сохраняют последовательный fallback; ошибочные значения отклоняются.", "",
    "CLI в отдельных процессах: 0–3 мс, checkpoint около 1,3 и 2,1 мс, цепочка **6→12→16**. Все 10 CSV и полный checkpoint совпали с непрерывным режимом 1. GUI тестирует выбор всех восьми режимов без изменения времени/состояния; три native-теста запускают реальные Win32/DX11 с `--compute-threads 6/12/16` и WM_CLOSE во время вычислений, подтверждают фактическое число и освобождение ресурсов. Это автоматизированные native-проверки, не ручной длительный сеанс.", "",
    f"[Проверка старой сборки](compatibility_final/report.json): старый checkpoint штатно отклонён новой сборкой; исходный файл не редактировался. 10 CSV короткого preset 4 совпали до/после. Вся отдельная проверка заняла {compat['elapsed_seconds']:.2f} с; она не является большим сетевым benchmark.", "",
    "## Измерения", "",
    f"{measure['host']['processor']}; {measure['host']['physical_cores']} физических ядер / {measure['host']['logical_cpus']} логических CPU. По три свежих последовательных процесса на каждый режим, один прогрев на нагрузку (не в медиане), порядок режимов циклически сдвигается между повторами. Другие тесты/сборки не выполнялись одновременно; сценарий проверяет отсутствие конкурирующих процессов и использует общий benchmark lock. Температура и сторонняя фоновая нагрузка не контролировались; affinity/priority не менялись. [Все команды, времена, SHA и побайтовые сравнения финальной сборки](measurements_final/runs.json), [сводка](measurements_final/summary.json).", "",
    "Quiet500 и burst500: те же 500 нейронов Ижикевича, 15 013 TM, dt=25 мкс, seed=706, запись 2 мс. Quiet — синтетический контроль без глобального шума, burst — σ=5. Medium128 — прежняя crossover-сеть 128 / 4096. Файлы взяты без изменений из `../20261019_parallel/fixtures/`; их SHA и параметры записаны в сводке. Этапы лабораторного плана не активируются.", "",
    "В таблице solve wall time, медиана [min; max], секунды; setup, экспорт, checkpoint и сравнение файлов исключены из solve. Полное процессное время и выборочное SD сохранены в JSON. Для всех ручных режимов факт равен запросу.", "",
    "| Запрос | Quiet500: 50 мс | Burst500: 50 мс | Medium128: 20 мс | Факт quiet/burst/medium |",
    "|---|---:|---:|---:|---|"
]
for count in COUNTS:
    rows = [next(r for r in measure["summary"] if r["workload"] == w and r["requested"] == count) for w in ("quiet500", "burst500", "medium128")]
    text = [f"{r['median']:.3f} [{r['min']:.3f}; {r['max']:.3f}]" for r in rows]
    lines.append("| " + (str(count) if count else "Авто") + " | " + " | ".join(text) + " | " + "/".join(str(r["effective"][0]) for r in rows) + " |")
lines += ["", "| Новый режим | × относительно 8, quiet | × относительно 8, burst | × относительно 8, medium |", "|---|---:|---:|---:|"]
for count in (6, 12, 16):
    values = [next(r for r in measure["summary"] if r["workload"] == w and r["requested"] == count)["speedup_over_8"] for w in ("quiet500", "burst500", "medium128")]
    lines.append(f"| {count} | " + " | ".join(f"{v:.3f}" for v in values) + " |")
lines += ["", "Значение × >1 означает быстрее 8, <1 — медленнее 8. Все измеренные прогоны и прогревы совпали с serial побайтно по 10 CSV, числовому дампу и полному checkpoint. Никакие поля checkpoint из сравнения не исключались.", "",
    "## Политика Авто и ограничения", "",
    "Обновлённая эвристика: неподдерживаемая сеть или <2048 синапсов → 1; 2048–4095 → целевой предел 4; **4096–8191 → предел 6**; ≥8192 → предел 8. Выбирается наибольшее поддерживаемое число, не превышающее предел и hardware_concurrency; неизвестное число → 1. Так, на машине с шестью доступными логическими CPU Auto не запрашивает восемь. Ручные 6/12/16 не ограничены эвристикой. Условие поддержки: только Izh/TM, без астроцитов. Выбор аппаратно зависим, 16 логических CPU не равны 16 физическим ядрам; оптимальность на каждой плотности внутри диапазона не доказана.", "",
    "Политика выбрана после отдельной [калибровки](measurements/summary.json): на 128/4096 медиана 6 потоков — 0,455 с против 0,528 с на 4, без пересечения диапазонов трёх повторов; поэтому средний диапазон переведён на 6. На большой сети 12 давали лишь около 1% выигрыша в quiet с перекрытием диапазонов и проигрывали в burst; 16 были существенно медленнее 8. Предел большой сети сохранён равным 8. Для 2048–4095 новых измерений в этой серии нет, сохранён прежний предел 4. После смены Auto заново выполнены весь штатный набор тестов и вся серия из 72 измерений; таблица выше относится только к финальной сборке. Калибровочные EXE сохранены в `build/thread-modes-calibration/Release`, SHA — `calibration_manifest.json`; их checkpoint не загружались финальной несовместимой сборкой.", "",
    "Финальная серия не скрыта при выборе политики: 12 потоков быстрее 8 по медианам quiet и burst (×1,122 и ×1,078), но интервалы перекрываются, а в калибровке burst 12 уступал 8 (4,208 против 4,086 с). Поэтому устойчивое преимущество 12 между сериями не установлено. На 128/4096 выигрыш 6 относительно 4 повторился: 0,504 против 0,564 с, диапазоны не перекрываются; финальный Auto действительно использовал 6 (0,499 с). Новая ручная настройка 12 позволяет выбирать её под конкретную нагрузку, автоматический предел большой сети остаётся 8. Самый медленный финальный burst16 — 15,811 с — включён в диапазон; причина этого отдельного замедления не измерялась.", "",
    f"Реальная длительность серии измерений: **{measure['elapsed_seconds']:.2f} с** ({measure['elapsed_seconds']/60:.2f} мин), включая запуск/экспорт/сравнения и прогревы; {len(measure['runs'])} измеренных процессов + {len(measure['warmups'])} прогрева. Суммарно {measure['total_model_seconds_measured']:.3f} модельной секунды в измеренных независимых прогонах и {measure['total_model_seconds_warmups']:.3f} с в прогревах. Длина одного прогона 50 или 20 мс. **150 модельных секунд не выполнялись**, экстраполяция на такой опыт не выдаётся за результат. Проверки протокола — 3 стадии по 1 мс. Паузы/закрытие используют короткие фактически достигнутые интервалы, хотя тест shutdown задаёт длинную конечную длительность.", "",
    f"Отдельно до финальной сборки: калибровочная серия заняла {calibration['elapsed_seconds']:.2f} с, также 72 измерения + 3 прогрева (2,88 + 0,12 модельных с). Калибровочная сборка + 32/32 CTest + smoke заняли {calibration_build['seconds']:.2f} с, из них CTest 261,20 с. Первые целевые 6/6 — {targeted['seconds']:.2f} с. Эти серии не заменяют друг друга и не складываются в один непрерывный модельный опыт. В последнем burst-повторе калибровки выросли несколько времён (включая Auto и serial); значения сохранены, выбросы не удалялись. Причина без мониторинга температуры/сторонней загрузки не установлена.", "",
    "Результаты не доказывают отсутствие всех гонок (ThreadSanitizer не применялся), оптимальность на другом CPU или научную достоверность модели. Убывающий выигрыш ограничен последовательными событиями и синхронизацией. Разница малых процентов при трёх повторах не считается убедительным универсальным преимуществом.", "",
    "## Сборка и воспроизведение", "",
    "Готовые исполняемые файлы и SHA-256:", ""]
for name in ("network_studio.exe", "network_studio_cli.exe"):
    entry = manifest["executables"][name]
    lines += [f"- `{entry['path']}` — `{entry['sha256']}`"]
lines += ["", "Все команды — из `E:/asp/26_27/configurator`. Не запускайте тесты одновременно с benchmark. Каталоги повторения должны быть новыми.", "", "```powershell",
    "$env:CTEST_PARALLEL_LEVEL='4'",
    "powershell -ExecutionPolicy Bypass -File .\\build.ps1 -Test -Smoke",
    "# Включить инструмент измерения в той же сборке (GUI/CLI/benchmark):",
    "cmake -S . -B build/native -DSTUDIO_BUILD_PERFORMANCE_TOOLS=ON -DSTUDIO_PROFILE=OFF",
    "powershell -ExecutionPolicy Bypass -File .\\build.ps1",
    "python -B performance/20261019_thread_modes/measure.py --out performance/20261019_thread_modes/reproduce --raw build/thread-modes-reproduce --repeat 3",
    ".\\build\\Release\\network_studio.exe --compute-threads 12",
    ".\\build\\Release\\network_studio_cli.exe --config performance/20261019_parallel/fixtures/burst_50ms.json --threads 16 --output build/example16",
    "```", "", "[Манифест сборки](manifest.json). `measure.py` требует обычный доступ Windows к списку процессов для проверки отсутствия конкурирующего расчёта. Старые checkpoint открывайте совместимым исходным EXE; новый режим потоков разрешено выбирать при resume внутри одной совместимой сборки."]
(HERE / "REPORT_RU.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("Report and manifest written")
