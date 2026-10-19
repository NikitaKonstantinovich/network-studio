# Методика проверки SimulationRunner

Этот каталог содержит проверку worker и GUI. Итоговые результаты — в
[REPORT_RU.md](REPORT_RU.md), контракт — в [ARCHITECTURE_RU.md](ARCHITECTURE_RU.md). Артефакты предыдущей серии
`../20261019_single_thread/` используются только для чтения.

Исходная проверка архивов и конфигураций сохранена в
[baseline_manifest.json](baseline_manifest.json), актуальная инвентаризация
оборудования — в [environment.json](environment.json). Хеши финальной сборки и архива исходников находятся в [build_manifest.json](build_manifest.json).

## Конфигурация и сборки

Использовать неизменённый `../20261019_single_thread/fixture.json`: 0,5 модельной
секунды, шаг 25 мкс, фиксированный seed. Это производная от сохранённого
`experiments/examples/benchmark.json`, а не найденный исходный опыт с точно
15 041 синапсом. В fixture 500 нейронов Ижикевича (400 E / 100 I), 15 013
синапсов Цодыкса—Маркрама, мультипликативная STDP и шум с интервалом 1 мс.
Происхождение и параметры зафиксированы в `../20261019_single_thread/provenance.json`.
Для короткого сравнения GUI допустим прежний `profile_fixture.json` (0,05 с),
но сравнивать длительности можно только при одинаковом fixture и его SHA-256.

Нужны Release-сборки без диагностических таймеров (`STUDIO_PROFILE=OFF`):

- Замороженная оптимизированная сборка до worker:
  `../20261019_single_thread/optimized-release/`.
- Новая сборка direct benchmark, runner benchmark и GUI из одного исходного
  состояния, с теми же компилятором, флагами и точностью чисел.

Имя новой цели: `network_studio_runner_benchmark`. Сборку и замеры
выполняет основной процесс задачи; эта методика сама их не запускает.
В итоговый отчёт необходимо добавить commit/dirty diff, архив исходников,
SHA-256 exe и fixture, checkpoint build ID, компилятор и флаги, CPU/RAM/OS,
режим питания и сведения о фоновой нагрузке. `run_runner.py` сохраняет часть
этих сведений автоматически, но строка `platform.processor()` не заменяет
точную модель процессора.

## Граница измерения

`network_studio_runner_benchmark CONFIG OUTPUTDIR [HZ=10] [CONSUMER_MS=1]`
создаёт `SimulationRunner`, устанавливает частоту публикации и ждёт начальный
`Barrier` вместе с соответствующим применённым снимком. Инициализация входит
в `setup_seconds`, а не в основной таймер.

Затем единственная команда `RunTo(duration)` запускает вычисление в worker.
Основная метрика `wall_seconds` идёт от отправки этой команды до получения
успешного подтверждения и применения конечного снимка: совпадают generation,
steps и time, sequence не меньше указанного в подтверждении, command ID уже
применён, `done=true`, `running=false`. Таким образом, ожидание доставки
конечного состояния включено. Каждый снимок последовательно применяется через
`SimulationView::apply`; нарушение цепочки delta завершается ошибкой.
Конечное модельное время дополнительно обязано равняться `config.duration`;
раннее завершение ручного этапа не считается успешным полным измерением.

Экспорт `result_*.csv`, сохранение `final.nscp` и запись профиля выполняются
отдельными командами worker **после** остановки основного таймера.
`export_and_checkpoint_seconds` измеряет экспорт и checkpoint вместе с
ожиданием подтверждений; профиль и shutdown в него не входят.

Сохраняются:

- worker compute/publish time и число публикаций;
- число полученных снимков, время применения delta на потребителе;
- число переданных sample/scalar/event/delivery/stimulus entries;
- максимальные наблюдавшиеся размеры пакета, retained sample data и очереди
  команд, а также число retained samples/events в конце;
- текущие суммарные счётчики нейронных спайков и астроцитарных выбросов.

`worker_compute_seconds` охватывает вызовы `Simulator::step`. Показатель
`worker_elapsed_seconds` считает активное wall time worker, включая команды
и публикации во время расчёта, но исключая паузы. Финальная принудительная
публикация уже после остановки входит в publish time и основной wall таймер,
но не в active elapsed. Поэтому эти времена нельзя складывать как независимые
части или напрямую вычитать одно из другого для точного разложения расходов.
`worker_model_per_second` нормирован на active elapsed, а не только на step time.

`consumer-ms` — ожидание между опросами, а не имитация CPU-затрат отрисовки.
Значение 0 делает yield без заданного сна. Фактическая задержка определяется
планировщиком ОС и может превышать заданный интервал.

## Что означает bounded display

Worker единолично владеет решателем. GUI получает неизменяемые пакеты и
применяет их к собственной модели отображения до начала кадра. Частота
публикации не задаёт шаг интегрирования. Ограничения `MaxDisplaySamples`,
`MaxDisplayDoubles`, `MaxDisplayJournal` относятся к передаче/отображению;
предел очереди команд задан отдельно в `SimulationRunner`.

В GUI может остаться только последнее окно истории. Пакеты событий могут
повторно передавать последнюю группу с одинаковым временем для корректной
замены границы, поэтому сумма delta entries не равна числу уникальных
научных событий. `display_was_truncated` и доступный интервал явно сохраняются
или подписываются; отсутствие точки на экране не означает её отсутствие
в authoritative history.

Экспорт и checkpoint используют полный доступный журнал worker, а не
retained GUI window. При включённой в самом опыте ограниченной записи
журналов экспорт тоже ограничен её правилами; worker не восстанавливает
данные, которые исходный решатель не сохранял. Полный checkpoint включает
всё состояние, предусмотренное его схемой, независимо от GUI retention.

`peak_snapshot_bytes_reported` — оценка, предоставленная runner, а не пик RSS.
`peak_retained_sample_bytes` считает только фактическое число сохранённых
double для времени/рядов/весов и не включает capacity, deque overhead,
конфигурацию или весь процесс. Ограничения display не являются доказательством
постоянного объёма всей памяти процесса: scientific history и протоколы
подчиняются собственным правилам хранения.

## Повторения и сравнение чисел

`../run_runner.py` запускает **одну** серию за вызов: `--mode runner`, `cli`
(direct solver benchmark) или `gui`. По умолчанию три свежих процесса.
Скрипт использует тот же `.benchmark.lock`, что `run.py`, не перезаписывает
существующий каталог серии и сохраняет команды, SHA-256, все времена,
медиану, min/max и выборочное стандартное отклонение. Измерения не запускаются
параллельно. Отдельный процесс профилировщика, тесты или тяжёлые фоновые задачи
во время измерения недопустимы. Три повторения показывают наблюдаемый разброс,
но сами по себе не дают статистической гарантии для любой системы.

При `--reference` указывается каталог **одного** готового прогона. Проверяются
точный набор и байты всех `result_*.csv`, а также `final.nscp`:

- По умолчанию `--checkpoint-mode exact`: совпадает весь checkpoint целиком,
  включая build ID и checksum. Это требование для direct/runner/GUI одной
  совместимой core-сборки и для разных частот публикации.
- Только если core build ID действительно различается, допускается явно
  выбрать `--checkpoint-mode build-metadata`: используется существующий
  офлайн `compare_checkpoint.py`. Он требует схему 4, одинаковые algorithm
  и serialized config, проверяет обе исходные FNV-1a контрольные суммы и
  сравнивает все остальные байты, исключая лишь поле build ID и зависящий
  от него итоговый checksum. Отличия метаданных сохраняются отдельно.

Ни один путь сравнения не загружает, не изменяет и не возобновляет checkpoint;
проверка совместимости штатного loader не отключается. Побайтовое равенство
доказывает равенство сериализованного результата этого опыта. Оно не заменяет
отдельные lifecycle/численные тесты и не доказывает корректность любой
непроверенной конфигурации. `numbers.bin` здесь не дублируется: полноту конечного
состояния, STDP traces/weights и очередей контролирует checkpoint.

## Пример последовательных команд

Из корня `configurator`, с новым свободным каталогом для каждой серии:

```powershell
$fixture = 'performance/20261019_single_thread/fixture.json'
$short = 'performance/20261019_single_thread/profile_fixture.json'
$frozen = 'performance/20261019_single_thread/optimized-release'
$new = 'build/runner/Release'

# Direct baseline; старые сохранённые измерения не переписываются.
python performance/run_runner.py --mode cli --exe "$frozen/network_studio_benchmark.exe" --fixture $fixture --out performance/20261019_runner/frozen_cli --repeat 3
python performance/run_runner.py --mode cli --exe "$new/network_studio_benchmark.exe" --fixture $fixture --out performance/20261019_runner/direct_cli --repeat 3 --reference performance/20261019_runner/frozen_cli/run_1

# Отдельные вызовы, один HZ за серию. Все сравниваются с тем же direct результатом.
python performance/run_runner.py --exe "$new/network_studio_runner_benchmark.exe" --fixture $fixture --out performance/20261019_runner/hz1 --hz 1 --repeat 3 --reference performance/20261019_runner/direct_cli/run_1
python performance/run_runner.py --exe "$new/network_studio_runner_benchmark.exe" --fixture $fixture --out performance/20261019_runner/hz10 --hz 10 --repeat 3 --reference performance/20261019_runner/direct_cli/run_1
python performance/run_runner.py --exe "$new/network_studio_runner_benchmark.exe" --fixture $fixture --out performance/20261019_runner/hz60 --hz 60 --repeat 3 --reference performance/20261019_runner/direct_cli/run_1

# GUI сравнивается отдельно на одном коротком fixture.
python performance/run_runner.py --mode gui --exe "$frozen/network_studio.exe" --fixture $short --out performance/20261019_runner/gui_before --repeat 3
python performance/run_runner.py --mode gui --exe "$new/network_studio.exe" --fixture $short --out performance/20261019_runner/gui_after --repeat 3 --gui-display-hz 10 --reference performance/20261019_runner/gui_before/run_1
```

Сначала сверить фактические пути новой сборки. При наличии уже измеренного
замороженного reference допустимо использовать его напрямую, сохранив путь
и SHA-256. Вызовы для всех частот — пример полного плана, а не обязательная
матрица каждого запуска. Для проверки влияния медленного потребителя
нужно провести отдельную явно подписанную серию с увеличенным `--consumer-ms`.

## Ограничения выводов о скорости

Direct `solve_seconds` измеряет цикл решателя. Runner `wall_seconds` также
включает обмен снимками и подтверждение потребителем. GUI `wall_seconds`
добавляет цикл UI/render/Present и политику occlusion. Их отношение следует
называть накладными расходами соответствующего режима, а не ускорением
математического ядра.

Hidden Win32/DX11 GUI benchmark не доказывает отзывчивость видимого интерфейса,
частоту кадров монитора или время работы GPU. В GUI-отчёте отдельно сохранить
renderer, Present status, occlusion sleeps, frame times и display Hz; для
вывода об отзывчивости нужна дополнительная проверка видимого GUI. Частота
публикации — целевая; worker может публиковать реже, а команды могут требовать
внеочередной публикации. Измеренные packet counts не следует автоматически
считать частотой кадров.

Итоговый `REPORT_RU.md` должен отделять подтверждённое совпадение чисел,
наблюдаемое время/разброс и ограничения эксперимента. Эта методика описывает протокол; фактически выполненная серия и результаты
перечислены в итоговом отчёте.
