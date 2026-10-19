# Общая проверка этапов: команды воспроизведения

Новая серия использует один неизменённый fixture на **50 модельных мс**,
фиксированные seed 706 и шаг 25 мкс. Всего 16 вариантов: direct 10, profile 2,
GUI 4; по 3 повтора, кроме исходного GUI с 1 повтором — **46 процессов**.
Команды ниже выполняются из корня `configurator`, строго последовательно.
Во время замеров нельзя запускать сборки, CTest, другие benchmarks или
массовое хеширование/чтение результатов. Архивные EXE не пересобираются.

Смысл сравнений, границы доказательств и происхождение контрольной версии:
[NOTES_RU.md](NOTES_RU.md). Машинный источник отчёта — `measured/runs.json`.
Отношение медиан само по себе не доказывает устойчивость небольшого эффекта.
Для cache-only генератор отдельно показывает изменение медианы и пересечение
диапазонов: выигрыш около 1,3% при пересекающихся диапазонах трёх повторов
не следует называть устойчивым. Он не объясняет весь эффект компактного ядра
или последующих изменений. Историческая таблица потоков на 0,5 с остаётся
отдельной; дополнительные измерения final1/final2 этой серией не добавляются.

## 1. Проверить и собрать отдельную контрольную версию

`control_no_parallel_roots` — новая абляция замороженной финальной реализации,
**не исторический P4**. Полный финальный source archive проверен по manifest
и извлечён в отдельный `source`; единственное изменение исходного кода:

```cpp
flow.event_workers=pool.get();  // финальная версия
flow.event_workers=nullptr;    // контроль: последовательный поиск корней
```

Остальные параллельные фазы сохраняются; контроль измеряется на 4 потоках.
Подтверждения: [provenance](control_no_parallel_roots/provenance.json),
[patch](control_no_parallel_roots/disable_parallel_roots.patch),
[build_result](control_no_parallel_roots/build_result.json).
Обычный checkpoint fingerprint должен измениться; compatibility не обходится.

Рецепт сборки уже подготовленной **отдельной копии**:

```powershell
& './performance/20261019_stage_review/control_no_parallel_roots/source/performance/build.ps1' -Build 'build/control' -Targets 'network_studio_benchmark'
```

Не перестраивайте сохранённый измеренный control EXE ради повторения замеров.
Для чистого воспроизведения восстановите финальный `source_snapshot.zip`
в новой рабочей копии, проверьте его manifest и 705 исходных файлов, примените
ровно указанную замену и сохраните новый build metadata. Отдельного скрипта
автоматического восстановления контроля здесь нет; производственные исходники
и `parallel-release` не должны изменяться.

## 2. Прочитать план, затем запустить измерения

Выходной каталог должен отсутствовать. Dry run ничего не измеряет:

```powershell
python -B performance/20261019_stage_review/run_stages.py --out performance/20261019_stage_review/measured_repro --repeat 3
```

После завершения всех сборок и проверок:

```powershell
python -B performance/20261019_stage_review/run_stages.py --out performance/20261019_stage_review/measured_repro --repeat 3 --execute
```

Driver проверяет fixture, archived EXE SHA где есть историческое свидетельство,
фактические время/шаги, режим profiling и численную идентичность. Для P1–P3
исторического SHA-манифеста нет: текущие архивные байты фиксируются явно без
приписывания им отсутствующего исходного снимка. Любая ошибка остаётся в
`failures`, успешным считается только `status=complete`. Сохранённые отчёты
сравнения содержат десять CSV и checkpoint; optional `numbers.bin` не считается
успешно сравнённым, когда файла нет.

`--select LABEL... --reference EXISTING_S0_RUN1` предназначены для отдельных
диагностических подсерий. Полный генератор итогового отчёта требует все 16
вариантов и ожидаемое число повторов; неполную серию он не публикует как итог.
Все аргументы: `python -B performance/20261019_stage_review/run_stages.py --help`.

## 3. Отдельно выполнить текущие тесты

Запускайте после завершения benchmark, не одновременно с ним. Используйте
новое имя отчёта, чтобы сохранить прежний `ctest.xml`:

```powershell
$stageReviewXml = Join-Path (Get-Location) 'performance/20261019_stage_review/ctest_repro.xml'
if (Test-Path -LiteralPath $stageReviewXml) { throw 'Choose a new JUnit report path' }
ctest --test-dir build/parallel --output-on-failure --parallel 1 --output-junit $stageReviewXml
```

Путь JUnit получается абсолютным от корня текущей рабочей копии.
Эти тесты относятся к текущей финальной сборке. Отсутствующие исторические
unit-test EXE P1–P3 не заменяются запуском текущих тестов.

## 4. Повторить проверку целостности после измерений

Скрипт проверяет исходники и EXE, использованные в сохранённой основной серии
`measured/`, связывает контрольную сборку с тремя измеренными запусками и
проверяет неизменность fixture и driver. Выполняйте только после benchmark:

```powershell
python -B performance/20261019_stage_review/validate_final.py --out performance/20261019_stage_review/final_validation_repro.json
```

Выберите отсутствующий выходной путь: прежний `final_validation.json` должен
сохраниться. Это повторное хеширование и проверка свидетельств; solver не
запускается. Скрипт читает основную серию `measured/`, а не `measured_repro/`.

## 5. Создать отчёт из завершённых свидетельств

Для уже сохранённой основной серии:

```powershell
python -B performance/20261019_stage_review/build_report.py --write
```

Для повторной серии и нового CTest XML:

```powershell
python -B performance/20261019_stage_review/build_report.py --manifest performance/20261019_stage_review/measured_repro/runs.json --tests performance/20261019_stage_review/ctest_repro.xml --write
```

Создаются `REPORT_RU.md` и `report_data.json`; без `--write` отчёт выводится
в stdout. Генератор читает небольшие JSON/XML, проверяет сохранённые результаты
сравнений и считает статистику, но не запускает solver, tests и хеширование
больших файлов. При активном `performance/.benchmark.lock`, неполной серии,
ошибке сравнения или результате CTest, отличном от 28/28, отчёт не создаётся.
