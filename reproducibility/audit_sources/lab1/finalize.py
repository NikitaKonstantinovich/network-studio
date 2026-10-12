"""Assemble compact evidence and findings without changing historical results."""
from pathlib import Path
import numpy as np
import audit


def main():
    audit.configure()
    out=audit.OUT
    base=audit.read(out/'baseline.json')
    independent=audit.read(out/'independent.json')
    user=audit.read(out/'user_comparison.json')
    metrics=audit.read(out/'analysis/metrics.json')
    probe=audit.read(out/'neuronet_protocol/probe_results.json')
    assert base['scenario_count']==45 and base['identical_csv_count']==90 and base['metrics_structurally_identical']
    for rows in independent['cases'].values():
        a,b=rows['native25us'],rows['native12_5us']
        assert a['same_spike_count'] and b['same_spike_count']
        assert b['max_event_error_ms']<=a['max_event_error_ms']+1e-8
        if 'native5us' in rows:
            assert rows['native5us']['max_event_error_ms']<=b['max_event_error_ms']+1e-8
    current_sha=audit.digest(audit.CLI)
    assert current_sha==base['cli_sha256']==independent['cli_sha256']==user['cli_sha256']==probe['studio_cli']['executable_sha256']
    inventory=[]
    for directory in [out/'raw',out/'extra',out/'neuronet_protocol']:
        for summary_path in sorted(directory.glob('*_summary.json')):
            prefix=Path(str(summary_path)[:-len('_summary.json')])
            s=audit.read(summary_path)
            assert s['complete'] and not s['error']
            csv_path=Path(str(prefix)+'_trace.csv')
            d=np.genfromtxt(csv_path,delimiter=',',names=True,encoding='utf-8-sig')
            assert all(np.isfinite(d[name]).all() for name in d.dtype.names)
            assert np.all(np.diff(d['time_s'])>0) and abs(d['time_s'][-1]-s['time_s'])<1e-9
            assert [len(audit.events(prefix,i+1)) for i in range(len(s['neuron_spikes']))]==s['neuron_spikes']
            inventory.append(dict(prefix=str(prefix.relative_to(out)),time_s=s['time_s'],steps=s['steps'],spikes=s['neuron_spikes'],
                trace_sha256=audit.digest(csv_path),events_sha256=audit.digest(Path(str(prefix)+'_events.csv')),
                configuration_sha256=audit.digest(Path(str(prefix)+'_experiment.json'))))
    audit.write(out/'run_inventory.json',inventory)
    assert len(inventory)==81
    original_provenance=audit.read(out/'analysis/run_provenance.json')
    assert all(audit.digest(audit.STUDIO/name)==sha for name,sha in original_provenance['source_files'].items())
    assert all(audit.digest(audit.LAB/'configs'/(name+'.json'))==sha for name,sha in original_provenance['config_sha256'].items())
    coverage=[
        dict(task='1',pages=[13,14],requirement='RS rest, single spike, synaptic Output',
             status='scientific behavior reproduced; Fire! UI action not repeated',
             evidence=['01_rest','02_single_pulse','02_reference_30x3ms'],
             findings='0 spontaneous spikes; one spike at101.75ms for30x1ms; Output jump10 and tau10ms. Textbook30x3ms produces two events.',
             external_limit='User report has no Fire! screenshot; chosen current pulse is not proof of identical Fire! semantics.'),
        dict(task='2',pages=[14],requirement='Autonomous spikes after changing neuron parameters',status='reproduced',
             evidence=['03_autonomous','fine_03_autonomous','03_autonomous_5us'],
             findings='b=.27,I=0,noise0:48 spikes/5s; late meanISI105.35946ms; original PNG approx106.894ms.',
             external_limit='Screenshot excludes neither all hidden inputs nor unknown numerical/history settings; raster resolution8.18ms/pixel.'),
        dict(task='3A',pages=[15,16],requirement='Choose stimulus to obtain2,10,20Hz',
             status='achieved using30x1ms; original30x3ms protocol has different spike multiplicity',
             evidence=['04_periodic_02hz','04_periodic_10hz','04_periodic_20hz','user_pulse_2hz','user_pulse_20hz'],
             findings='6/30/60 spikes for6/30/60 adapted pulses. Original-source screenshots show Output envelopes; they do not resolve close membrane spikes.',
             external_limit='At original30x3ms, numerical method/step change firing. Exact original-protocol equality is not established.'),
        dict(task='3B',pages=[16],requirement='Find constant input supporting repetitive spiking',status='reproduced including userI3.8',
             evidence=['sweep_dc_3p80','05_dc_4','user_dc3p8_12p5us'],
             findings='I3.8:29 spikes/5s,lateISI178.05ms,PNGapprox177.728ms. I4:36spikes/5s. Swept3.75 quiet late,3.8 sustained.',
             external_limit='Grid bracket3.75..3.8 is protocol-specific; local equilibrium stability loss3.7975 does not by itself prove global firing threshold.'),
        dict(task='4',pages=[16,17],requirement='Global and individual noise at multiple sigma values',status='reproduced qualitatively',
             evidence=['06_noise_global_0','06_noise_global_5','06_noise_global_8','07_noise_individual','repeat_noise','user_noise_global_4p0_*','user_noise_global_4p5_*'],
             findings='User sigma4/4.5:mean2.2667/3.5111Hz in1..10s across2neuronsx5seeds. Individual/global equal sigma gives identical traces; sigma0 overrides global8.',
             external_limit='NeuroNet random algorithm/seed/noise update interval and long event logs are unavailable; quantitative noise equivalence is unverified.')
    ]
    findings=dict(date='2026-10-12',laboratory=1,
        conclusion='All four scientific tasks are covered; current CLI preserves historical results. Independent deterministic and TM checks pass. Full NeuroNet numerical equivalence is not established.',
        provenance=dict(cli=str(audit.CLI),cli_sha256=current_sha,
            pdf_sha256=audit.digest(Path('C:/Users/N1kit/Downloads/UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf')),
            production_sources_sha256=original_provenance['source_files'],
            audit_sources_sha256={p.name:audit.digest(p) for p in out.glob('*.py')},
            historical_inputs_unchanged=True,historical_results_rewritten=False,
            source_pages=[5,6,7,12,13,14,15,16,17],
            user_report=user['source_report'],user_evidence_sha256=user['source_sha256']),
        counts=dict(native_runs=len(inventory),historical_scenarios=45,additional_native_runs=36,
                    historical_CSV_byte_identical=90,historical_CSV_total=90,
                    independent_DOP853_configurations=14,additional_Radau_crosscheck=1,
                    analytic_TM_sample_rows=16002,
                    translated_Euler_diagnostic_runs=4,
                    previously_saved_87_case_audit='reviewed, not rerun as a full suite'),
        validation=dict(all_runs_complete_finite_monotonic_counts_consistent=True,
            historical_metrics_identical=True,seed_replay_identical=metrics['checks']['noise_replay_identical'],
            deterministic_count_agreement=True,dt_refinement_nonincreasing_error=True,
            max_TM_abs_error=max(max(v['max_absolute_error'].values()) for v in independent['TM_analytic'].values()),
            original_NeuroNet_binary_executed_this_audit=False),
        coverage=coverage,baseline_convergence=base['convergence'],
        adaptive_convergence=independent['cases'],noise_replicates=base['noise_replicates'],
        noise_user_protocol=user['noise'],user_screenshot_comparison=user,
        isolated_pulse_Radau_vs_DOP853=independent['cases']['02_reference_30x3ms']['radau_vs_dop853'],
        limitations=[
            'Functional reproduction of2/10/20Hz uses30x1ms; it must not be described as the original30x3ms input.',
            'Output envelope repetition is not a reliable count of individual membrane spikes when responses overlap.',
            'Original current binary screenshots document one10Hz control at two steps; this is not validation of every historical screenshot.',
            'User screenshots lack complete step, initial state, prior history, global noise, and exported event logs.',
            'Noise is independent Gaussian sample-and-hold1ms in this simulator; matching sigma does not establish matching stochastic processes.',
            'RK4 subthreshold integration uses grid-end resets; event phase error accumulates and is not globally fourth order.',
            'This Lab1 audit does not validate astrocytes or STDP; Fire! and original GUI operations were not executed.',
            'Translated Euler results are diagnostic Python binary64 calculations, not fresh NeuroNet execution or bitwise x87 reproduction.'
        ],evidence_files=['baseline.json','analysis/metrics.json','independent.json','user_comparison.json','run_inventory.json','neuronet_protocol/probe_results.json'])
    audit.write(out/'findings.json',findings)
    c=independent['cases']
    rows=[]
    for name in ['04_periodic_20hz','03_autonomous','05_dc_4','02_reference_30x3ms','reference_20hz_30x3ms']:
        v=c[name]
        rows.append(f"| `{name}` | {v['native25us']['actual_spikes']} | {v['native25us']['max_event_error_ms']:.6f} | {v['native12_5us']['max_event_error_ms']:.6f} | "+(f"{v['native5us']['max_event_error_ms']:.6f}" if 'native5us' in v else '—')+' |')
    noise=user['noise']
    content=f'''# Повторная проверка ЛР1 — 12 октября 2026

**Научные режимы всех четырёх заданий воспроизведены.** Выполнен 81 настоящий запуск текущего Release CLI: 45 исторических сценариев и 36 дополнительных проверок. Все 90 прежних CSV состояний/событий совпали побайтово, все прежние метрики совпали структурно. Проверены завершение, конечность состояний, часы и соответствие счётчиков журналам во всех 81 запусках. Исторические файлы не перезаписывались. CLI SHA-256: `{current_sha}`.

Основания: методичка, с. 5–7 и 12–17; [пользовательский отчёт и семь исходных PNG](../../../../../../labs/lab1_report/Отчёт_ЛР1.md); [историческое сравнение с оригиналом](../../../lab1/results/neuronet_comparison/README.md). Полная карта заданий, хеши и ограничения — [findings.json](findings.json), каждый фактический запуск — [run_inventory.json](run_inventory.json).

## Покрытие заданий и сравнение с опытами пользователя

| Задание | Проверенный результат | Сравнение с оригиналом |
|---|---|---|
| 1. RS, покой, одиночный ответ, Output | При I=0 — 0 спайков. Импульс 30×1 мс — один спайк в 101,75 мс; выходной скачок g·y=10, τ=10 мс | В пользовательском отчёте нет отдельного кадра Fire!; текущий опыт проверяет эквивалентный одиночный ответ, не реализацию команды Fire! |
| 2. Автоколебания | b=0,27, I=0: 48 спайков/5 с; поздний ISI=105,359 мс | Из PNG получается ≈106,9 мс; масштаб ≈8,18 мс/пиксель. Стимулятор и полные настройки в кадре не видны |
| 3А. 2/10/20 Гц | Подобранный 30×1 мс даёт 6/30/60 спайков на 6/30/60 импульсов; частоты по ISI=2,0000/9,9995/19,9854 Гц | В исходных PNG 2/20 Гц применён 30×3 мс. Это другой протокол; интервалы огибающих Output не доказывают по одному спайку |
| 3Б. Постоянный ток | Пользовательское I=3,8: 29 спайков/5 с, поздний ISI=178,05 мс; при I=4: 36 спайков/5 с | PNG I=3,8 даёт ≈177,7 мс при ≈8,16 мс/пиксель: количественно согласуется в пределах разрешения изображения |
| 4. Шум | Проверены σ=0/4/4,5 из пользовательского отчёта и прежние 0/5/8; общий и индивидуальный режимы, пять seed | Качественно согласуется; количественное совпадение с NeuroNet не установлено из-за неизвестного шумового процесса и отсутствия длинных исходных журналов |

При σ=4/4,5 выполнено по пять запусков с двумя независимыми нейронами, длительность 10 с, окно анализа 1–10 с. Средняя частота ± SD по десяти траекториям: **{noise['4.0']['mean_rate_hz']:.3f}±{noise['4.0']['sd_across_neuron_seed_hz']:.3f}** и **{noise['4.5']['mean_rate_hz']:.3f}±{noise['4.5']['sd_across_neuron_seed_hz']:.3f} Гц**. При σ=0 спайков нет. Явное индивидуальное σ даёт побайтово те же трассы, что наследуемое общее σ, при том же seed/индексе. Индивидуальное σ=0 при общем σ=8 полностью отключает шум выбранного нейрона (0 против 54 спайков у второго за 5 с). Удержание шума — 1 мс; это не непрерывная СДУ.

## Независимая численная проверка

14 детерминистических конфигураций независимо проинтегрированы DOP853 с точными границами тока, локализацией v=30 и сбросом. Во всех сравнениях число спайков совпало. Таблица показывает максимальную разницу времени соответствующих событий относительно DOP853, а не разницу двух сеток:

| Опыт | Спайков | dt=25 мкс, ошибка мс | 12,5 мкс | 5 мкс |
|---|---:|---:|---:|---:|
{chr(10).join(rows)}

Изолированный 30×3 мс вызывает два спайка с задержками 1,498652255 и 3,156522502 мс. Radau отличается от DOP853 менее чем на 3·10⁻¹² мс; при dt=1 мкс ошибка CLI=0,0004775 мс. Аналитическое решение линейной TM-динамики и скачков сопоставлено с 16 002 строками CSV; максимальная абсолютная ошибка u/x/y=1,06·10⁻¹³. Это проверка синапса на фактических native временах спайков, отдельно от ошибки нейронного решателя.

Сетка постоянных токов даёт позднюю устойчивую генерацию при 3,8 и её отсутствие при 3,75. Линеаризация непрерывных подпороговых уравнений даёт потерю устойчивости стационарного состояния при I=3,7975. Это полезное независимое согласование, но не доказательство глобального порога с учётом сброса. При b=0,27 дискриминант стационарного уравнения −0,0271: вещественного равновесия нет.

## Что выяснилось для исходного 30×3 мс

При исходном основном протоколе (старт 100 мс, стимуляция 3 с) число спайков при 2/10/20 Гц равно **12/60/81** на **6/30/60** импульсов. Последний режим чередует одиночные и двойные ответы: нельзя распространять удвоение при 10 Гц на все частоты.

Дополнительно повторены видимые в пользовательских PNG фазы: 2 Гц, 50–53 мс; 20 Гц, 20–23 мс. Начальное состояние явно выбрано V=−70,u=0, шум=0, длительность 5 с; это не восстановление неизвестного состояния ручной сессии. На позднем окне 1–5 с:

| Протокол | CLI RK4 25 мкс | Диагностический перевод Euler 0,5 мс |
|---|---:|---:|
| 2 Гц | 16 спайков / 8 периодов: по два | 16 / 8: по два |
| 20 Гц | 107 / 80: 53 одиночных и 27 двойных периодов | 80 / 80: по одному |

Для 20 Гц DOP853 подтверждает все 133 спайка полного 5-секундного опыта. Ошибка времени CLI уменьшается **2,008→0,333→0,075 мс** при 25→12,5→5 мкс. Перевод Euler — дополнительная диагностика по ранее исследованному бинарному алгоритму, **не запуск оригинального NeuroNet**; Step на пользовательских кадрах этих опытов не показан. Исходные Output-кадры не разрешают близкие спайки и подтверждают периодичность огибающей. Поэтому совпадение огибающих не доказывает совпадение спайковых журналов.

Ранее полученные реальные Em-кадры оригинального NeuroNet подтверждают переход от одного ответа к двум при уменьшении шага 0,5→0,025 мс в отдельном 10-Гц опыте. Текущий повтор его перевода и CLI снова даёт 20/40/40 событий на окне 1–3 с. Frequency=20/40 в оригинале относится к окну счётчика 50 мс и не равна долговременной частоте. Ни эти кадры, ни наш перевод не заменяют исходные временные ряды для остальных заданий.

**Вывод о конфигураторе:** проверенные уравнения, сброс, TM и воспроизводимость сохранены; новых ошибок модели в ЛР1 не обнаружено. Точные времена ограничены сбросом в конце шага, поэтому фазовая ошибка накапливается. Полной количественной эквивалентности оригинальному NeuroNet не заявляем. Астроциты и STDP этой проверкой не валидируются; интерфейсные действия оригинала заново не выполнялись. Полный прежний аудит из 87 случаев прочитан, но не выдан за повторённую серию.

Результаты: [сводные метрики](analysis/metrics.json), [побайтовое сравнение](baseline.json), [независимые решатели](independent.json), [пользовательские протоколы](user_comparison.json), [рисунки](figures/), [проверка 10-Гц протокола](neuronet_protocol/probe_results.json). Воспроизведение: последовательно `audit.py baseline`, `audit.py independent`, `user_comparison.py`, запуск `lab1/neuronet_probe.py --output <этот каталог>/neuronet_protocol`, затем `finalize.py`; NumPy/SciPy/Matplotlib/Pillow берутся из указанного PYTHONPATH.
'''
    (out/'findings.md').write_text(content,encoding='utf-8')
    print('Finalized:',len(inventory),'native runs; all validation gates PASS')


if __name__=='__main__':main()
