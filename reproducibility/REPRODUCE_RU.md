# Как повторить аудит ЛР1–3

Команды ниже выполняются в PowerShell из `configurator`. Аудит использует текущую Release-сборку и сохранённые исходные конфигурации. Он не запускает оригинальный NeuroNet: его файлы и изображения служат внешними свидетельствами.

## Окружение и входные данные

```powershell
Set-Location E:/asp/26_27/configurator
$auditPython = 'C:/Users/N1kit/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$auditDeps = 'E:/asp/26_27/tmp/python_deps'
$env:PYTHONPATH = $auditDeps
$env:PYTHONDONTWRITEBYTECODE = '1'
$ErrorActionPreference = 'Stop'
function Invoke-AuditPython {
    & $auditPython -B @args
    if ($LASTEXITCODE -ne 0) { throw 'Этап аудита завершился ошибкой; не продолжайте цепочку.' }
}
```

Нужны NumPy, SciPy, Matplotlib, Pillow, PyMuPDF и ReportLab; для PDF используются `C:/Windows/Fonts/arial.ttf` и `arialbd.ttf`. Для сборки C++ нужны CMake, MSVC C++ Build Tools и Windows SDK, обнаруживаемые `build.ps1`.

Проверить наличие источников:

- Методичка: `C:/Users/N1kit/Downloads/UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf`.
- Пользовательские `E:/asp/26_27/labs/lab{1,2,3}_report/Отчёт_ЛР{1,2,3}.md` и папки `images`.
- Оригинальные `.net`, `.log`, матрицы и снимки в `C:/Users/N1kit/Downloads/1_NeuroNet`; для ЛР3 — вложенная папка `1_NeuroNet/NET_files/STP`.
- Исторические `lab1/configs`, `lab2/configs`, `lab3/configs`, их метрики и **старые сырые CSV**: `lab1/results/raw`, `lab2/results/rings/raw`, `lab2/results/populations/raw`, `lab3/results/raw`. Эти большие каталоги могут отсутствовать после одного `git clone`. Без них проверка побайтового совпадения с архивом не выполняется.

При переносе адаптировать абсолютные пути в `preflight.py`, аудитах ЛР1/ЛР2/ЛР3, `lab3/extract_neuronet_reference.py` и путь шрифтов в `build_report.py`. Номер коммита сам по себе не идентифицирует сборку с рабочими изменениями: сверять SHA в `evidence/provenance.json` и лабораторных JSON. Пока идут расчёты, не пересобирать бинарные файлы и не менять исходные конфигурации.

## Полный новый расчёт

Все аудиторы пишут рядом со своим `.py`. Рабочий каталог этой серии — `verification/repro_labs_20261012`. Перед новым полным запуском **сохранить существующий каталог под другим именем**: часть исполнителей запрещает перезапись, другие её допускают. Не смешивать старые и новые результаты.

`freeze_evidence.py` сохраняет исполняемые копии аудиторов в `reproducibility/audit_sources/lab1`, `lab2`, `lab3`. Для повторения всей цепочки восстановить их в новый пустой рабочий каталог:

```powershell
$auditRun = Join-Path (Get-Location) 'verification/repro_labs_20261012'
if (Test-Path -LiteralPath $auditRun) {
    throw 'Сначала сохраните прежний каталог аудита под другим именем.'
}
foreach ($lab in 'lab1', 'lab2', 'lab3') {
    $destination = Join-Path $auditRun $lab
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath "reproducibility/audit_sources/$lab" -Filter '*.py' |
        Copy-Item -Destination $destination
}
```

Запуск копий прямо из `audit_sources` тоже возможен, но результаты тогда окажутся **в `audit_sources`**. Общие `preflight`, `plot_audit`, `compose_report` и `freeze_evidence` читают фиксированный каталог `verification/repro_labs_20261012`; приведённая выше схема сохраняет согласованные пути.

Собрать текущую версию, записать тесты и происхождение входных данных:

```powershell
.\build.ps1 -Smoke
& ctest --test-dir build/native -C Release --output-on-failure 2>&1 |
    Tee-Object -FilePath "$auditRun/ctest.log"
if ($LASTEXITCODE -ne 0) { throw 'CTest не пройден.' }
Invoke-AuditPython reproducibility/preflight.py
```

`preflight.py` требует уже существующий `ctest.log`, фиксирует хеши бинарных файлов, исходников, PDF и пользовательских отчётов; также визуализирует нужные страницы методички. При проверке точно той же имеющейся сборки можно пропустить её пересборку, сохранив шаг CTest и provenance.

ЛР1 — архивный повтор, независимые решатели, дополнительные пользовательские протоколы, затем сводка:

```powershell
Invoke-AuditPython "$auditRun/lab1/audit.py" baseline
Invoke-AuditPython "$auditRun/lab1/audit.py" independent
Invoke-AuditPython "$auditRun/lab1/user_comparison.py"
Invoke-AuditPython lab1/neuronet_probe.py --output "$auditRun/lab1/neuronet_protocol"
Invoke-AuditPython "$auditRun/lab1/finalize.py"
```

ЛР2 — архивные кольца/популяции, независимые метрики, сравнение исполнителей, импорт исходных графов и контроль фактического 11-нейронного кольца:

```powershell
Invoke-AuditPython "$auditRun/lab2/audit.py" --python-deps $auditDeps
Invoke-AuditPython "$auditRun/lab2/independent_check.py"
Invoke-AuditPython "$auditRun/lab2/details.py"
Invoke-AuditPython "$auditRun/lab2/imported_graphs.py" --python-deps $auditDeps
Invoke-AuditPython "$auditRun/lab2/ring11.py"
Invoke-AuditPython "$auditRun/lab2/report.py"
```

ЛР3 — частотные опыты, короткая чувствительность сетевого расчёта к шагу, три полных сетевых опыта и сборка выводов:

```powershell
Invoke-AuditPython "$auditRun/lab3/audit.py"
Invoke-AuditPython "$auditRun/lab3/network_audit.py" --short-sensitivity
Invoke-AuditPython "$auditRun/lab3/network_audit.py"
Invoke-AuditPython "$auditRun/lab3/assemble_findings.py"
```

Длинные 500-нейронные серии и особенно три 30-секундных опыта основного ядра требуют заметного времени; ждать завершения процесса и проверки всех результатов. `network_audit.py --benchmark-only` — необязательная оценка скорости на 0,2 с, не замена полной серии. Скрипты не должны выдавать частично рассчитанную сеть за завершённый опыт.

Для уже имеющихся сырых результатов доступны `--skip-run`: у ЛР2 это `audit.py`, `details.py`, `imported_graphs.py`; у ЛР3 — `audit.py`, `network_audit.py` (в том числе вместе с `--short-sensitivity`). Они повторно анализируют данные; это **не новый нативный расчёт**. Сохранять исходный provenance: некоторые сводки при повторном анализе обновляются. В ЛР1 `finalize.py` проверяет готовые результаты без новых запусков.

## Необязательный контроль крайне малых чисел

Отдельный FTZ/DAZ-исполнитель проверяет влияние обработки субнормальных величин на скорость и результаты. Он не заменяет штатный CLI. Его исходник и инструкции сохранены в [audit_sources/lab3/ftz/README.md](audit_sources/lab3/ftz/README.md). Для нового рабочего каталога скопировать также эту подпапку:

```powershell
Copy-Item -LiteralPath reproducibility/audit_sources/lab3/ftz -Destination "$auditRun/lab3/ftz" -Recurse
```

Сборка и запуск описаны в указанном README. После завершения **обеих** серий выполнить `ftz_audit.py --skip-run`, чтобы сравнение включало все три частоты, а не только готовые на момент диагностики. `available=false` означает отсутствие сравнения. Успешный выход сравнителя сам по себе не означает равенство: проверить поля событий, весов и всех записанных состояний, величину и принадлежность каждого расхождения. Результаты остаются в `ftz_findings.json`, отдельно от основной серии. Время исполнения под параллельной нагрузкой — наблюдение, а не строгий тест ускорения.

## Сводный отчёт и компактные доказательства

После завершения всех лабораторий:

```powershell
Invoke-AuditPython reproducibility/plot_audit.py
Invoke-AuditPython reproducibility/compose_network.py
Invoke-AuditPython reproducibility/compose_report.py
Invoke-AuditPython reproducibility/build_report.py
Invoke-AuditPython reproducibility/freeze_evidence.py
```

`plot_audit.py` строит рисунок из новых метрик ЛР3. `compose_network.py` требует три завершённых расчёта исходной сборки в `lab3/network_findings.json` и создаёт `network_report_blocks.json`. `compose_report.py` требует `lab1/findings.json`, `lab2/replay_summary.json`, `lab3/task1_findings.json` и эти сетевые блоки. Часть чисел и выводов в `compose_report.py` и `compose_network.py` написана явно: после изменения модели/протокола сверить их с новыми findings, а не считать текст автоматически пересчитанным.

`build_report.py` создаёт `REPORT_RU.md`, `output/pdf/LABS_1_3_REPRODUCIBILITY_RU.pdf` и изображения страниц `qa/page_*.png`. Просмотреть страницы: отсутствие ошибки компиляции не гарантирует отсутствие обрезанного текста или неудачных переносов.

Если нужен только повторный PDF из сохранённых `report_content.json` и `figures`, достаточно `Invoke-AuditPython reproducibility/build_report.py`; новый расчёт и `preflight` не требуются.

`freeze_evidence.py` проверяет наличие трёх завершённых сетевых результатов ЛР3 и findings всех лабораторий; если запускалась FTZ-диагностика — её четыре завершённых опыта и доступность всех сравнений. Затем копирует верхнеуровневые JSON в `evidence`, аудиторы `.py` — в `audit_sources`, исходник/описание FTZ-исполнителя, общий provenance и CTest. `evidence/manifest.json` содержит размеры и SHA копий. **CSV и вложенные подробные JSON не архивируются этой командой**: они остаются в рабочем каталоге аудита; сохранить его отдельно для полного повторного анализа.

| Материал | Расположение относительно `network_studio` |
|---|---|
| Итоговые Markdown и PDF | `reproducibility/REPORT_RU.md`, `reproducibility/output/pdf/` |
| Компактные выводы, хеши и тестовый журнал | `reproducibility/evidence/` |
| Сохранённые исходники аудиторов | `reproducibility/audit_sources/` |
| Все новые CSV, конфигурации и детальные проверки | `verification/repro_labs_20261012/lab1`, `lab2`, `lab3` |
| Оригинальные архивные опыты конфигуратора | `lab1/results`, `lab2/results`, `lab3/results` |
| Внешние свидетельства NeuroNet | PDF/`.net`/`.log` в Downloads и пользовательские `labs/*_report` |

Совпадение CSV проверяет повторяемость собственного расчёта. Независимые ODE/TM-проверки оценивают точность реализации, а сравнение с NeuroNet ограничено полнотой его исходных данных и различиями протоколов; эти три вида доказательств не взаимозаменяемы.
