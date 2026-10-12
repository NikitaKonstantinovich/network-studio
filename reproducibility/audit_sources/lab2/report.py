"""Build the Lab 2 audit findings from completed, separately stored runs."""
from __future__ import annotations
import csv, math, statistics, sys
from pathlib import Path
sys.dont_write_bytecode=True
from details import HERE, STUDIO, ORIGINAL, load, save, sha

def main():
    replay=load(HERE/'replay_summary.json')
    metrics=load(HERE/'populations/metrics.json')
    rings=load(HERE/'rings/metrics.json')
    independent=load(HERE/'independent_metrics.json')
    bridge=load(HERE/'bridge_500/comparison.json')
    n11=load(HERE/'ring11/findings.json');n11_fine=load(HERE/'ring11/fine_findings.json')
    imported=load(HERE/'imported_graphs/results/populations/metrics.json')
    imported_independent=load(HERE/'imported_graphs/results/populations/independent_metrics.json')
    evidence=load(HERE/'neuronet_evidence.json')
    graphs=load(STUDIO/'lab2/results/populations/graphs.json')
    assert replay['native_runs_completed']==58 and replay['csv_files_byte_identical']==159
    assert replay['all_ring_metrics_identical'] and replay['all_population_metrics_identical']
    assert independent['passed'] and bridge['passed'] and all(x['passed'] for x in imported_independent.values())
    for path,h in replay['binary_sha256'].items():assert sha(path)==h
    runs=metrics['runs']
    by_degree={}
    for k in [15,30]:
        by_degree[k]={metric:dict(mean=statistics.mean([runs[f'k{k}_seed{seed}'][metric] for seed in [11,42,101]]),
             sample_sd=statistics.stdev([runs[f'k{k}_seed{seed}'][metric] for seed in [11,42,101]]))
             for metric in ['comparison_mean_rate_hz','comparison_burst_rate_hz']}
    convergence={}
    def events(name):
        with (HERE/'rings/raw'/f'{name}_events.csv').open(newline='') as f:
            return [(float(r['time_s']),int(r['node_index_1based'])) for r in csv.DictReader(f)]
    for name,case in rings.items():
        if case['group']=='primary' or name=='exp_n10_w0p7':
            fine=rings[name+'_dt12p5us'];a,b=events(name),events(name+'_dt12p5us')
            assert [n for _,n in a]==[n for _,n in b]
            convergence[name]=dict(counts_equal=case['counts']==fine['counts'],
                circulation_equal=case['sustained_circulation_to_5s']==fine['sustained_circulation_to_5s'],
                maximum_ordinal_event_difference_ms=max(abs(x[0]-y[0])*1000 for x,y in zip(a,b)))
    source_report=STUDIO.parents[2]/'labs/lab2_report/Отчёт_ЛР2.md'
    image_dir=source_report.parent/'images'
    images=['ring3_a.png','ring3_b.png','ring5_scheme.png','ring5_activity.png','ring10_w1.png','ring10_w07.png',
        'ring10_w05.png','ring10_w02.png','net15_histogram.png','net30_histogram.png','net15_raster.png',
        'net30_raster.png','net15_frequency.png','net30_frequency.png','net15_bursts.png','net30_bursts.png']
    source_pdf=Path('C:/Users/N1kit/Downloads/UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf')
    coverage=[
        dict(task='1',pages='17–18',request='Unidirectional excitatory rings N=3..10, w=1, learning rate0, manual start; determine which circulate.',
            executed='8 main +8 fine-step rings; controls overlap tasks1–2. Added N11 at two step sizes after checking source image node count.',status='All requested sizes simulated; native TM N3..10 one passage then quiet. Empirical NeuroNet N3/N5 quiet. Source image labeled N10,w1 actually has11nodes; nativeN11 circulates too, period differs.'),
        dict(task='2',pages='18',request='N10 ring w=.7,.5,.2,0; measure circulating periods.',
            executed='4 additional main +4 fine-step; w1 fromtask1;15 exponential controls and3noise controls plus1fine circulating control.',
            status='All weights covered. Native TM quiet in all cases; no invented TM period. Exp w.7 sustains49.525ms but is a different protocol. Original NeuroNet report estimates82/97ms atw.7/.5; w0 not performed there.'),
        dict(task='3a (first 3)',pages='18–20',request='Generate500neurons400E/100I; weights.5±.1; Gaussian incoming degree30±3; inspect histograms.',
            executed='8distinct saved graphs independently checked; degree15/30 across3seeds plusnearest controls. Additional exact imports of11891/5577edge NeuroNet graphs.',
            status='Topology and distributions checked; owngenerator satisfies stated sampling convention. OriginalNeuroNetneighborhood algorithm yields different realized degrees.'),
        dict(task='3b (second 3)',pages='20–22',request='Compare15±2 and30±3 using rasters, population frequency50ms and burst frequency5s, threshold>5Hz, learningrate0.',
            executed='15saved runs:6main,2nearest,4step,2quiet,1repeat; seed42main100s. Plus2actualgraph30scontrols.',
            status='All observables and numeric comparisons produced; independent event-bin analysis passes. Qualitative bursts reproduced, actual NeuroNet≈1Hz burst rate not matched quantitatively.')]
    findings=dict(date='2026-10-12',status='Native replay verified; partial empirical NeuroNet reproduction',
        native_runs=dict(saved_replay=58,rings=43,populations=15,short_core_population_bridge=2,imported_graph_controls=2,
            n11_controls=2,total_documented_records=64,exploratory_n11_repeat_calls=1,total_process_invocations=65),
        replay=replay,independent_metric_checks=dict(graphs=8,population_runs=15,imported_runs=2,passed=True),
        coverage=coverage,ring_convergence=convergence,ring_primary_results={n:c for n,c in rings.items() if c['group']=='primary'},
        ring_control= rings['exp_n10_w0p7'],ring_noise_control={n:c for n,c in rings.items() if c['noise_sigma']>0},
        population_seed_comparison=by_degree,population_step_convergence=metrics['checks'],
        generated_seed42_graphs={k:graphs[k] for k in ['k15_seed42','k30_seed42']},
        imported_graph_results=imported,gui_core_bridge=bridge,n11_control=n11,n11_half_step_control=n11_fine,
        original_neuronet=dict(report_path=str(source_report),report_sha256=sha(source_report),
            screenshot_sha256={str(image_dir/name):sha(image_dir/name) for name in images},
            observations=dict(rings='N3/N5 transients. N10 regular activity atw.7/.5 andquietat.2. Image labeledN10,w1 actually has11nodes, independently consistent with ring_N10_w1.net. Approxperiods71(N11)/82(N10)/97(N10)ms fromsourceauthor manual measurement; no rawspikeexport used.',
                population='Snapshot11891edges,400E100I,32burstsat29970ms; snapshot5577edges,400E100I,29burstsat29185ms. Images show10-s rasterwindows withapproximatelyten populationevents, not enough to infer an exactlong-runmean or robustmonotonicbursttrend.',
                limits='No new NeuroNet simulation. Snapshotstate is not an eventlog. Named ring_N10_w1.net has11neurons. Remaining N4/N6–9 were not empirically tested in the source report.'),
            evidence_file='neuronet_evidence.json'),
        delay_evidence=evidence['delay_matrix'],
        known_protocol_differences=['Our RK4dt.025ms vs originalvisibleStep.500ms; oldbinaryaudit confirmsEuler.',
            'Our isolated1ms30unit input substitutes Fire!; original startup state/manualFire exacttrace unavailable.',
            'Our explicitconductiondelay iszero; original shows Sp.Velocity.050 and outputhistorybuffers. Exported dls values belongto anothergraph; ringdelayunits/indexformula not established.',
            'Samegraphcontrols importonlytopology,E/I labels,weights; no continuationofstate,delays,originalnoiseorSpikeShowingbookkeeping.',
            'Our noiseindependentGaussian held1ms,seed42; originalσ3alone doesnotidentify identicalsamples.',
            'Our burst extraction usesexplicitplateauand50mspeakdistance convention; NeuroNet exactdetector not reconstructed.'],
        stale_documentation=['Historical lab2 README/SHORT_REPORT claimGUI16-neuronlimit; currentlimit1000neurons/50000synapses; oldreports intentionally untouched.'],
        limitations=['No proof that all TM rings fail in everyparameterregime; onlythe specified finite5sprotocol.',
            'No causal attribution of the NeuroNet difference to delay alone.',
            'Populationstep convergence concernsaggregateobservables, not identicalindividualspike trains.',
            '500Ncore bridge .2s only, not100sGUI rerun; history records4neurons andall events.'],
        provenance=dict(pdf_path=str(source_pdf),pdf_sha256=sha(source_pdf),pdf_pages='17–22',
            scripts_sha256={p.name:sha(p) for p in HERE.glob('*.py')},sources_unchanged=replay['changed_saved_inputs']==[]))
    save(HERE/'findings.json',findings)
    text='''# ЛР2: аудит воспроизводимости от 12 октября 2026

**Все сохранённые численные опыты повторены без изменений результата: 43 кольца и 15 популяционных сценариев, 159 из 159 CSV побайтово совпали.** Пересчитанные метрики также совпали. Это подтверждает повторяемость наших расчётов, но не точную эквивалентность NeuroNet.

Подготовлены результаты **58 повторов и 6 контрольных записей**: два исполнителя одного 500N графа, два расчёта на точных графах исходных опытов NeuroNet и кольцо N=11 на двух шагах. Сохранённые конфигурации и старые отчёты не изменялись. [Машинные результаты](findings.json), [сверка 58 прогонов](replay_summary.json), [команды, SHA и длительности](run_manifest.json).

## Покрытие методички, с.17–22

| Задание | Что проверено | Итог и граница |
|---|---|---|
| 1: кольца N=3–10, w=1 | Все восемь размеров; повтор с вдвое меньшим шагом | TM даёт один проход и затухание. Методичка спрашивает, во всех ли ансамблях возникает циркуляция, и не требует положительного ответа для каждого N. |
| 2: N=10, w=0.7/0.5/0.2/0 | Все четыре веса плюс базовый w=1; контроль шага; варианты без STP и с шумом | TM не поддерживает циркуляцию в выбранном протоколе. Период для отсутствующей активности не вычислялся. |
| 3а: 500 нейронов, 400E/100I, распределения | Восемь различных сохранённых графов, входные степени и модули весов; независимый пересчёт CSV | Распределения соответствуют принятому алгоритму выборки. Найденные исходные графы NeuroNet имеют другое фактическое число связей. |
| 3б: сравнение 15±2 и 30±3 | Растры, частота в окне 50 мс, пачки выше 5 Гц и частота пачек в окне 5 с; три seed, nearest-контроль, шаг, шум, повтор | Все запрошенные характеристики получены. Пачечная активность есть; количественная частота NeuroNet пока не воспроизведена. |

В методичке два разных пункта имеют номер 3; здесь они разделены на 3а и 3б. В исходном описании аудита предполагалось 41/17 опытов, но проверенные манифесты и файлы содержат **43/15**, всего те же 58.

## Кольца: полученные числа и эмпирический эталон

Наш протокол: RS `a=.02,b=.2,c=−65,d=8`; TM `g=20,U=.5,τI=10 мс,τrec=50 мс,τfac=1000 мс`; STDP выключена; шум 0; один импульс `30×1 мс` первому нейрону в 0.1 с; RK4 с шагом 25 мкс; длительность 5 с; явных задержек проведения нет.

| N=10, вес | Наша TM-модель | Реальные снимки/отчёт NeuroNet |
|---:|---|---|
| 1 | По одному спайку; последний 137.125 мс | Снимок подписан N=10, но содержит **11 нейронов**; ≈71 мс нельзя считать периодом N=10 |
| 0.7 | По одному спайку; последний 155.275 мс | Регулярная активность; ≈82 мс |
| 0.5 | Спайк только N1, 101.750 мс | Регулярная активность; ≈97 мс |
| 0.2 | Спайк только N1 | Затухание |
| 0 | Спайк только N1 | В исходном отчёте опыт не выполнен |

Для N=3–10, w=1 каждый нейрон в нашей TM-модели спайкует ровно один раз, после чего сеть молчит до 5 с. Исходные NeuroNet-снимки N=3 и N=5 также показывают затухание; N=4 и N=6–9 в том отчёте не представлены. Числа 71/82/97 мс — приближённые измерения автора по растру, а не извлечённые нами точные времена событий. **Для N=10 при w=.7/.5 есть эмпирическое качественное расхождение**, но оно само по себе не устанавливает ошибку уравнений или единственную причину.

Повторный визуальный пересчёт `ring10_w1.png` выявил **11 узлов**, что согласуется с содержимым `ring_N10_w1.net`. Дополнительный контроль с единственным динамическим изменением — N=11 вместо N=10 — **поддерживает TM-циркуляцию и в нашем ядре**. За 5 с числа событий `[86,86,85,85,85,85,85,85,85,85,85]`, последний спайк 4.99615 с; все поздние переходы идут по кольцу. Период N1 в последнюю секунду 57.625 мс (17.35358 Гц по ISI, 18 событий в окне). На шаге 12.5 мкс числа событий и режим сохраняются, период 57.6125 мс, максимальный сдвиг парных событий 1.2125 мс. Таким образом, **переход N=10→11 меняет режим без подгонки остальных параметров**. Для фактического 11-узлового снимка качественная циркуляция воспроизведена, но численный период ≈71 мс не совпал. [Контроль N11](ring11/findings.json), [половинный шаг](ring11/fine_findings.json).

Контроль без STP использует экспоненциальный ток и полный выброс `s+=1`. Для N=10,w=.7 он сохраняет упорядоченную циркуляцию до 5 с: период 49.525 мс, частота по ISI 20.19182 Гц; в последнюю секунду у N1 20 событий. Это другой режим, а не успешное воспроизведение TM-кольца. При w=1 даже этот контроль затухает после короткого всплеска. Шум σ=3 вызывает дополнительную активность, которую нельзя автоматически считать циркуляцией после одиночного запуска.

На шаге 12.5 мкс все 12 основных TM-опытов сохранили числа спайков и затухание; максимальный сдвиг сопоставленных событий — 0.075 мс. Для циркулирующего экспоненциального контроля период 49.5125 мс; сдвиг события за 5 с достигает 1.4125 мс. Критерий циркуляции требует событий у всех узлов после входа, не менее двух событий на узел в последнюю секунду, не менее 95% переходов к следующему узлу и последнего события не раньше 4.9 с.

Состояния нашего N10 TM,w1 при первом возврате указывают на адаптацию: перед входом `u≈−13.9254`, перед возвратом `u≈−10.5148`; входящий синапс ещё имеет `x=1`. Возврат остаётся подпороговым. Это объяснение поведения нашей записи, не доказанная причина различия с NeuroNet.

## 500 нейронов: повтор, устойчивость и импорт исходных графов

Окно сравнения 5–30 с, среднее ± выборочное стандартное отклонение по seed 11,42,101:

| Заданная степень | Средняя частота на нейрон, Гц | Частота пачек, Гц |
|---|---:|---:|
| 15±2 | 28.72688 ± 0.44194 | 4.93333 ± 0.02309 |
| 30±3 | 46.64541 ± 0.38643 | 4.20000 ± 0.04000 |

При seed=42 основные записи длятся 100 с; остальные основные seed — 30 с. Nearest-контроль сохраняет направление: 27.84448/46.56720 Гц на нейрон и 4.68/4.12 пачек/с. Уменьшение шага вдвое на 10-секундных записях изменяет среднюю частоту на 0.189%/0.337%, оценки пачек остаются 5.0/4.2 Гц. Индивидуальные спайковые последовательности при этом различаются. При нулевом шуме и отсутствии внешнего запуска обе сети молчат. Идентичный повтор даёт побайтово одинаковые события и состояния.

Независимый анализ стандартной библиотекой Python повторно проверил восемь графов и все 15 сетевых записей: счётчики событий, 1-мс разбиение, причинное окно 50 мс, локальные максимумы/плато, подавление соседних пиков, окно пачек 5 с, конечные числа и завершение. [Результаты проверки](independent_metrics.json). Детектор пачек явно задан; он не объявляется реконструкцией недокументированного детектора NeuroNet.

В реальном [отчёте NeuroNet](../../../../../labs/lab2_report/Отчёт_ЛР2.md) найдены снимки сетей и соответствующие `.net`. Число связей и средняя входная степень подтверждены непосредственно файлами:

| Граф | Связи | Средняя входная степень | Модуль веса, среднее ± SD |
|---|---:|---:|---:|
| Наш 15±2, seed42 | 7 519 | 15.038 | 0.498008 ± 0.098273 |
| NeuroNet «15±2» | 5 577 | 11.154 | 0.505650 ± 0.095522 |
| Наш 30±3, seed42 | 15 017 | 30.034 | 0.499352 ± 0.099446 |
| NeuroNet «30±3» | 11 891 | 23.782 | 0.499073 ± 0.103129 |

NeuroNet-мастер использовал соседство с density=2; одинаковые введённые средние не означают одинаковый алгоритм или получившийся граф. На растрах видны популяционные пачки порядка одной в секунду; у более плотной сети частотные пики выходят за шкалу 100 Гц, у менее плотной лежат примерно в области 60–70 Гц. Снимки/состояния фиксируют 32 пачки при 29.970 с и 29 при 29.185 с. Это не точные средние частоты без знания истории обнуления счётчиков и не проверка устойчивого направления изменения пачек между ансамблями.

Чтобы снять различие графов, выполнены **два новых 30-секундных контроля** с точными связями и весами из `.net`. Тип источника задаёт знак веса; нейроны перезапущены из покоя, TM-пулы сброшены, использованы наш шум σ=3/seed42/hold1мс, RK4 25 мкс и нулевые задержки. Это импорт графа, не продолжение состояния NeuroNet.

| Импортированный граф | Частота на нейрон (5–30 с), Гц | Пачек/с (5–30 с) | Максимум частоты 50 мс, Гц |
|---|---:|---:|---:|
| 5 577 связей | 20.88232 | 5.32 | 82.72 |
| 11 891 связь | 36.22872 | 5.04 | 151.84 |

Оба контроля прошли независимый пересчёт. Конкретный граф меняет результаты, но его совпадения недостаточно для совпадения динамики NeuroNet. [Метрики](imported_graphs/results/populations/metrics.json), [происхождение и правила импорта](imported_graphs/source_provenance.json).

## Что установлено о численном протоколе и задержках

На оригинальных снимках/логах видны Step=0.500 мс, Spike showing=1, Sp.Velocity=0.050, learning rate=0. Для колец Noise=0, для больших сетей Noise=3. Ранее статически исследованный бинарный файл NeuroNet SM R05 использует явный Euler для Izhikevich; наш расчёт использует RK4 0.025 мс. Одна цифра Noise=3 не задаёт тот же генератор, seed и временной интервал обновления.

В кольцевых `.net` имеются буферы `Output_deque`: например для w=.7/.5 размеры 5–8 элементов. Это сохранённая история выходов; точная задержка каждого ребра из длины такого буфера не выводилась. Найденная матрица `dls_0.0005.txt` содержит 5915 положительных значений **0.5…7**, среднее **2.254776**, и 31 нулевое значение в ячейках существующих рёбер. Все положительные ячейки соответствуют связям её снимка. Однако матрица относится к **другому 500N графу на 5946 связях**, а не к кольцам или графам 5577/11891. Файл не содержит метаданных единиц. Формула индекса очереди и преобразование Sp.Velocity не восстановлены, поэтому числа не подписаны как точные задержки колец в миллисекундах.

Само существование разных численных шагов, схемы запуска, истории выходов и протоколов шума установлено. Их отдельный причинный вклад не измерен. [Извлечённые поля и SHA исходников](neuronet_evidence.json). Имена `.net` ненадёжны: `ring_N10_w1.net` содержит 11 нейронов; в анализе числа взяты из содержимого.

## Текущий GUI и основной движок

Старая фраза в `lab2/README.md` и `SHORT_REPORT_RU.md` о лимите 16 нейронов устарела: текущий конфигуратор поддерживает 1000 нейронов и 50 000 связей. Эти старые отчёты намеренно не переписывались в ходе аудита.

Один и тот же граф 500N/15 017 связей рассчитан основным `network_studio_cli` и быстрым `network_studio_population` на 0.2 с: **все 5588 событий совпали по индексам и времени**, 201 отсчёт N1–N4 дал максимальную разницу V `3.25×10⁻¹²` мВ и u `6.39×10⁻¹⁴`. Длительности исполнения около 11.2 с и 0.26 с соответственно. [Сравнение](bridge_500/comparison.json), [полная конфигурация для GUI](bridge_500/config.json). События сравнивались для всех 500 узлов; подробная запись была включена для четырёх.

Это проверка общего ядра на реальном размере сети, дополнительно к прежнему послойному 24-сценарному тесту малых графов. Она не является новым 100-секундным GUI-прогоном. Для длинных однородных сетей без STDP, астроцитов и задержек пакетный исполнитель существенно быстрее; полная GUI-конфигурация содержит сам граф, а одиночный population-base JSON служит только шаблоном.

## Повтор аудита

Из каталога `standalone/network_studio` (Python с NumPy, SciPy, Matplotlib):

```powershell
python -B verification/repro_labs_20261012/lab2/audit.py --python-deps E:/asp/26_27/tmp/python_deps
python -B verification/repro_labs_20261012/lab2/independent_check.py
python -B verification/repro_labs_20261012/lab2/details.py
python -B verification/repro_labs_20261012/lab2/imported_graphs.py --python-deps E:/asp/26_27/tmp/python_deps
python -B verification/repro_labs_20261012/lab2/ring11.py
python -B verification/repro_labs_20261012/lab2/report.py
```

`audit.py`, `details.py` и `imported_graphs.py` поддерживают `--skip-run` для проверки имеющихся результатов с контрольными суммами. `details.py`/`imported_graphs.py` используют указанные в них доступные локальные пути оригинальных NeuroNet-файлов. Все новые выходы создаются в этом каталоге. SHA бинарных файлов, сценариев, PDF, снимков и результатов приведены в JSON. Нативный повтор 58 опытов занял около 234 с при двух параллельных процессах.
'''
    # Relative path goes from verification/repro_labs_20261012/lab2 to workspace/labs.
    text=text.replace('../../../../../labs/lab2_report/','../../../../../../labs/lab2_report/')
    (HERE/'findings.md').write_text(text,encoding='utf-8')
    print('Findings written; documented records64, saved CSV159/159, all checks PASS',flush=True)

if __name__=='__main__':main()
