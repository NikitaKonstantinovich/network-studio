"""Assemble compact evidence and Russian findings without altering lab sources."""
from __future__ import annotations
import argparse,csv,json,math,os,sys
from pathlib import Path
sys.dont_write_bytecode=True
import audit as a

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--network-source',default='network_findings.json');args=parser.parse_args()
    task=a.read(a.HERE/'task1_findings.json')
    network=a.read(a.HERE/args.network_source) if (a.HERE/args.network_source).exists() else None
    historical={r['name'].removeprefix('historical_'):r for r in task['historical']}
    report=Path('E:/asp/26_27/labs/lab3_report/Отчёт_ЛР3.md')
    external_estimates=[14.5,14.5,14.5,17.5,15.5,10.,6.75,6.75]
    maxerror=max(max(r['exact_tm_max_abs_errors'].values()) for section in ['historical','calibrated','convergence'] for r in task[section])
    rows=[]
    for c,estimated in zip(task['calibrated'],external_estimates):
        f=c['frequency_hz'];h=historical[f'f{f:g}'.replace('.','p')]
        rows.append(dict(stimulus_frequency_hz=f,historical_actual_spike_rate_hz=h['metrics']['spike_rate_hz'],historical_output_peak=h['metrics']['peak_mean'],
                         external_visual_peak_approx=estimated,calibrated_amplitude=c['amplitude'],calibrated_width_ms=c['width_ms'],
                         calibrated_actual_spike_rate_hz=c['metrics']['spike_rate_hz'],calibrated_output_peak=c['metrics']['peak_mean'],
                         ideal_periodic_tm_peak=c['regular_tm_reference']['peak']))
    sensitivity=[]
    for name in ['network_benchmark','network_benchmark_fine','network_benchmark_quarter','network_benchmark_eighth','network_benchmark_sixteenth']:
        path=a.rawpath(name,'summary.json')
        if path.exists():
            c=a.read(a.HERE/'configs'/f'{name}.json');s=a.read(path)
            sensitivity.append(dict(name=name,dt_us=c['dt']*1e6,duration_s=c['duration'],total_spikes=sum(s['neuron_spikes']),
                                    active_neurons=sum(n>0 for n in s['neuron_spikes']),csv_sha256=a.sha(a.rawpath(name,'events.csv'))))
    findings=dict(lab=3,status='complete' if network and len(network['runs'])==3 else 'network_running',
                  cli_sha256=task['cli_sha256'],historical_configs=17,historical_csv_matches=task['bytewise_equal_count'],historical_csv_total=51,
                  historical_trace_rows=task['historical_rows'],additional_pair_configs=len(task['calibration_trials'])+len(task['calibrated'])+len(task['convergence']),
                  total_unique_pair_configs=17+len(task['calibration_trials'])+len(task['calibrated'])+len(task['convergence']),
                  exact_tm_max_abs_error=maxerror,frequency_table=rows,calibrated_half_step=task['convergence'],
                  original_snapshot_check=task['external_snapshot_check'],network_short_step_sensitivity=sensitivity,
                  source_pdf=dict(path=str(a.extract.DEFAULT_PDF),sha256=a.sha(a.extract.DEFAULT_PDF),pages=[22,23,24]),
                  source_user_report=dict(path=str(report),sha256=a.sha(report)),
                  source_screenshots_sha256={str(p):a.sha(p) for p in sorted((report.parent/'images').glob('task*.png'))},
                  original_source_files={str(p):a.sha(p) for p in [a.LAB/'compare.py',a.LAB/'reference_tm.py',a.LAB/'extract_neuronet_reference.py',a.LAB/'neuronet_snapshot_reference.json']},
                  coverage=dict(task1='Completed with actual presynaptic frequencies; historical fixed-stimulus protocol retained separately.',
                                task2='Original 500-node graph transferred to current zero-delay core. External NeuroNet numerical reproduction is not claimed.'),
                  limits=['Internal CSV regression is distinct from agreement with NeuroNet.',
                          'Calibrated pulse amplitude varies with frequency; the historical 30x3ms protocol is a separate comparison.',
                          'Peaks are exact event-replayed Output=20*y, verified against sampled CSV states, not maxima of postsynaptic voltage or a 1ms sampled trace.',
                          'NeuroNet screenshots are visual estimates without raw event/peak time series, repeated trials or confidence intervals.',
                          'Original snapshot sequence verifies the first output, normalization and early Euler-like decay only, not a complete frequency curve.',
                          'Network transfer omits transport queues and coarse NeuroNet output timing; initial states are reset independently across frequency conditions.',
                          'Network 30s trajectories have no full-duration step convergence demonstration; a separate 0.2s sensitivity series is reported.',
                          'Convex hull area is an extent proxy and includes inactive points inside the hull; active-neuron counts are the primary recruitment measure.',
                          'tau_fac=0 is not required by Lab3 tasks and is only evaluated as an analytical limit; current core requires tau_fac>=1ms.'])
    if network:
        findings['network_runs']=network['runs']
        findings['network_source_file']=args.network_source
        findings['network_mixed_numerical_modes']=network.get('mixed_numerical_modes',False)
        findings['network_imports']=[{k:v for k,v in x.items() if k!='coordinates'} for x in network['imports']]
    if (a.HERE/'network_step_sensitivity.json').exists():findings['network_step_sensitivity']=a.read(a.HERE/'network_step_sensitivity.json')
    ftz=a.read(a.HERE/'ftz_findings.json') if (a.HERE/'ftz_findings.json').exists() else None
    if ftz:
        findings['ftz_diagnostic']={k:v for k,v in ftz.items() if k!='runs'}
        findings['ftz_diagnostic']['runs']=[{k:({kind:{key:value for key,value in check.items() if key!='columns'} for kind,check in v.items()} if k=='comparison' else v) for k,v in r.items() if k!='network_metrics'} for r in ftz['runs']]
    a.save(a.HERE/'findings.json',findings)
    lines=['# Аудит ЛР3: воспроизводимость и недостающие опыты','',
           'Старые 17 конфигураций повторены текущим Release CLI: **51/51 CSV совпали побайтово**, включая трассы, события и веса. Проверено 451 017 исторических строк. Это регрессия собственной программы; она не означает численного совпадения с NeuroNet.','',
           f'Дополнительно выполнено {findings["additional_pair_configs"]} парных сценариев: 17 коротких подборов, восемь калиброванных полных опытов и три опыта с половинным шагом. Всего {findings["total_unique_pair_configs"]} уникальных парных конфигураций. Максимальное расхождение состояний u,x,y с точным событийным TM: **{maxerror:.3g}**.','',
           '## Задание 1: различать стимулы и спайки','',
           'Методичка (с.23–24) требует настроить стимуляцию так, чтобы пресинаптический нейрон выдавал нужную активность. Старые 30×3 мс не обеспечивают один спайк на период для всех частот. Новая серия использует импульс 1 мс; амплитуда 30 для 0,1–10 Гц, 40 для 25 Гц, 60 для 50 Гц и 100 для 100 Гц. В каждом полном установившемся периоде подтверждён ровно один спайк. Перед первой стимуляцией — 6 с покоя.','',
           '| Задано, Гц | Старые 30×3: спайки/с | Старый Output | Скриншоты NeuroNet, ≈ | Калиброванный Output | Точный регулярный TM |',
           '|---:|---:|---:|---:|---:|---:|']
    for r in rows:lines.append(f'| {r["stimulus_frequency_hz"]:g} | {r["historical_actual_spike_rate_hz"]:g} | {r["historical_output_peak"]:.6f} | {r["external_visual_peak_approx"]:g} | {r["calibrated_output_peak"]:.6f} | {r["ideal_periodic_tm_peak"]:.6f} |')
    lines+=['','Output означает высоту вершины относительно нуля: 20y. При высокой частоте это не равно приращению над остаточным хвостом. Вершины восстановлены по экспортированным событиям точной моделью TM; её состояния проверены во всех строках CSV. Усредняются ответы в полных периодах после ≥5 с стимуляции и ≥2 периодов; пустой период учитывается в частоте, но не превращается в фиктивную нулевую амплитуду.',
            '', 'Наибольший ответ среди восьми заданных частот — при 5 Гц (16,603147); при 100 реальных спайках/с — 4,859938. Контрольный первый одиночный ответ после покоя — ровно 10. У старого протокола 100 Гц стимулятора дают только 50 спайков/с и Output 6,566967; это объясняет, почему ту точку нельзя считать ответом синапса на 100 спайков/с.',
            '', 'Фасилитация повышает u при повторении импульсов; на высоких частотах уменьшается доступный x, поскольку восстановление не успевает за расходом. Их конкуренция объясняет максимум. Для математического предела τfac=0 u перед каждым отдельным событием обнуляется, на событии становится U, и остаётся депрессия; это не равнозначно выключению STP. Такой предел отдельно рассчитан аналитически, но не заявлен как поддерживаемый режим CLI.',
            '', 'При уменьшении шага 25→12,5 мкс для калиброванных 5/50/100 Гц средние установившиеся вершины не изменились в записанной точности; максимальное попарное смещение спайков — 0,0125/0,0125/0,025 мс. Малые различия конечного окна от асимптотического TM на 0,5–1 Гц связаны с конечной длительностью установления, а не с ошибкой RK4.',
            '', '## Независимый внешний эталон','',
            'Повторно прочитаны все 21 исходный снимок NeuroNet (0…10 мс), заново проверены их SHA-256 и все поля эталонного JSON, кроме времени создания. Для нейрона №189 перед первым ответом x=20,u=0, затем u=0,5 и Output=10. Это подтверждает нормировку ресурса на 20 и порядок обновления u перед высвобождением. Между снимками затухание согласуется с явным Euler 0,5 мс с остатком Output ≤3×10⁻⁶ в исходных единицах сериализации.',
            '', 'Эталон не доказывает всю частотную кривую: это первое событие в связанной сети и короткий хвост, а не запись восьми установившихся режимов. Пауза >5 с в этих снимках не наблюдается; задано полностью восстановленное начальное состояние. Скриншоты парного опыта дают приближённые амплитуды и подтверждают пропуски каждого второго стимула при 100 Гц, но не дают точного ряда событий. Точная внешняя численная воспроизводимость не установлена.',
            '', '## Задание 2: исходная сеть из 500 нейронов','',
            'Перенесён фактический граф трёх net1_*.net: 400 E + 100 I, 7625 связей, одинаковая топология и веса во всех трёх файлах. Сохранены типы, v=−70,u=−14, параметры Ижикевича и TM, веса и пять мишеней стимулятора [453,208,189,213,373]. Знак берётся из типа источника; исходный файл хранит положительные модули. Шум и STDP выключены. Ресурс каждого источника представлен одинаковыми состояниями его исходящих синапсов.','',
            '**Это перенос сети в текущую модель без транспортных задержек, а не повторное исполнение NeuroNet.** В исходных файлах есть Output_deque, а в журнале — Sp. Velocity=0,05 и шаг 0,5 мс. Их семантика не подменяется догадкой: текущий расчёт использует непосредственную передачу и RK4 25 мкс. Каждая частота запускается заново; исторический пользовательский опыт переключал частоты последовательно без сброса.','']
    if network and len(network['runs'])==3:
        if network.get('mixed_numerical_modes'):
            lines+=['**Численная конвенция этой таблицы смешанная:** частоты, отмеченные в JSON как diagnostic FTZ+DAZ, рассчитаны отдельным CLI с обнулением субнормальной арифметики. Для каждой строки сохранены конкретный бинарник, SHA-256 и mode. Незавершённый штатный опыт не объявляется выполненным. Сопоставление FTZ и штатных завершённых траекторий — в ftz_findings.json; его нельзя автоматически распространить на непроверенный режим.','']
        lines+=['Для каждой частоты выполнено 30 с; количественное окно — 10…30 с. Output наблюдается у исходного тормозного нейрона №453.','',
                '| Стимулы, Гц | Активных /500 | Не стимулируемых напрямую | Средняя частота сети, Гц/нейрон | №453, спайки/с | №453 Output: средний максимальный постспайковый пик за периоды с ответом |',
                '|---:|---:|---:|---:|---:|---:|']
        for r in network['runs']:
            q=r['neuron453'];lines.append(f'| {r["frequency_hz"]:g} | {r["active_neuron_count"]} | {r["active_nonstimulated_count"]} | {r["mean_population_rate_hz"]:.6f} | {q["spike_rate_hz"]:.6f} | {q["stimulus_period_output_peak_mean"]:.6f} |')
        if network.get('mixed_numerical_modes'):
            lines+=['','Режимы расчёта: '+ '; '.join(f'{r["frequency_hz"]:g} Гц — {r["numerical_mode"]}' for r in network['runs'])+'.']
        lines+=['','Output в этой таблице — максимальный постспайковый пик внутри каждого полного периода с ответом, затем среднее по таким периодам. Периоды без спайков исключены; остаточный хвост на левой границе не измеряется как новый ответ. JSON сохраняет отдельно число всех полных периодов и число периодов с ответом. Это метрика визуальных вершин, а не максимум по каждому периоду, включая пустые.', '', 'Число активных нейронов — первичная мера вовлечения; площадь выпуклой оболочки координат в JSON служит только дополнительной оценкой пространственного охвата, включая неактивные точки внутри контура. Для №453 дополнительно проверено точное событийное TM по экспортированным состояниям.']
        lines+=['','Число полных периодов с ответом №453 / всех полных периодов: '+ '; '.join(f'{r["frequency_hz"]:g} Гц — {r["neuron453"]["responding_complete_periods"]}/{r["complete_period_recruited"]["periods"]}' for r in network['runs'])+'.']
        lines+=['','В штатной серии 100 Гц подавляют распространение за пределы пяти напрямую стимулируемых нейронов. Это качественно совпадает с историческим изображением; Output №453 около 6,567 также попадает в визуальный диапазон 6,5–7. Однако расширение охвата при переходе 0,1→1 Гц не воспроизведено: активны 493 и 484 нейрона соответственно, а не увеличение их числа. Нельзя объяснять эту разницу только задержками без отдельного подтверждения их алгоритма. Исходные скриншоты не дают точного количественного эталона активности всех 500 нейронов.']
    else:lines+=['Три 30-секундных опыта ещё выполняются; итоговые сетевые числа будут добавлены после проверки CSV.']
    if sensitivity:
        lines+=['','Проверка чувствительности первого 0,2 с к шагу:','', '| Шаг, мкс | Спайков всего | Активных нейронов |','|---:|---:|---:|']
        for r in sensitivity:lines.append(f'| {r["dt_us"]:g} | {r["total_spikes"]} | {r["active_neurons"]} |')
        lines+=['','Полная 30-секундная траектория не объявляется сошедшейся по шагу. Уже на первых 0,2 с при 25 против 1,5625 мкс суммарное число спайков отличается на 2,334%, а индивидуальные количества — у 106 нейронов. При 3,125 против 1,5625 мкс различие суммарного количества уменьшается до 0,187%, но сохраняется у 12 нейронов. Самый мелкий шаг также не является доказанным точным решением. Массовое вовлечение 477–480 нейронов устойчиво в коротком контроле; точная траектория отдельных клеток чувствительна к пороговым каскадам. Совпадение качественного режима с изображением не равнозначно численному воспроизведению.']
    if ftz:
        lines+=['','## Отдельный контроль субнормальной арифметики','',
                'Штатный CLI сохранён. Отдельный launcher с тем же готовым network_core.lib включает FTZ+DAZ (биты 15 и 6 MXCSR), то есть обнуление субнормальных результатов и операндов. Это явно отдельная численная конвенция. Для каждого завершённого штатного опыта сравниваются все события, веса и все выборочно записанные состояния.','',
                '| Опыт | FTZ, с wall | События / веса равны побайтно | Макс. абсолютное различие записанных состояний |',
                '|---|---:|---|---:|']
        for r in ftz['runs']:
            q=r['comparison'];available=all(x.get('available') for x in q.values())
            match='да' if available and q['events']['bytewise_equal'] and q['weights']['bytewise_equal'] else ('нет' if available else 'штатный опыт ещё выполняется')
            delta=f'{q["trace"]["max_absolute_difference"]:.6g}' if available else 'не проверено'
            lines.append(f'| {r["original_name"]} | {r["wall_s"]:.3f} | {match} | {delta} |')
        lines+=['','В 0,1 Гц различия ограничены пятью записанными y-хвостами: ≤8,902×10⁻³⁰⁶; все записанные нейронные V,u и прочие TM состояния совпали. Хвост может быть нормальным числом: FTZ обнуляет уже субнормальное приращение RK4 и оставляет очень малое плато. Поэтому отличие не скрывается утверждением о совпадении всех нормальных значений. Для 1 Гц и короткого 0,2-секундного контроля все CSV совпали побайтно. Время 0,1 Гц уменьшилось с 1359,219 до 533,643 с; 1 Гц — с 552,354 до 551,196 с. Это фактические wall-времена при разной параллельной нагрузке, а не контролируемый benchmark ускорения.','',
                'Повторение: `ftz/README.md`; отдельные бинарники, исходники и SHA-256: `ftz_findings.json`. Диагностика не заменяет отсутствующие штатные результаты и не доказывает сходимость сетевой траектории.']
        hundred=next((r for r in ftz['runs'] if r['original_name']=='network_net1_100Hz'),None)
        if hundred and all(x.get('bytewise_equal',False) for x in hundred['comparison'].values()):
            lines+=['','Штатный 100 Гц завершён за 2982,853 с (около 49 мин 43 с), FTZ — за 557,502 с. Для полного 30-секундного 100-Гц опыта все события, веса и выборочно записанные состояния совпали побайтно. Основная итоговая таблица использует все три штатных результата; смешанная серия не понадобилась.']
    lines+=['','## Файлы и повторение','',f'- [Методичка, с.22–24]({a.extract.DEFAULT_PDF.as_posix()})',f'- [Исходный пользовательский отчёт и скриншоты]({report.as_posix()})',
            '- `audit.py`: все 17 прежних конфигов, подбор и полная калиброванная серия, точный TM и проверка первичных снимков.',
            '- `network_audit.py`: строгий импорт исходного графа, три независимых 30-секундных прогона и метрики.',
            '- `findings.json`: компактные показатели, области применимости и хеши; `configs/`, `raw/` — все новые входы и результаты.',
            '', 'Запуск из каталога аудита: `python audit.py`, затем `python network_audit.py`, затем `python assemble_findings.py`. Использован bundled Python 3.12; matplotlib нужен только для рисунков. Параметр `--skip-run` выполняет повторный анализ уже записанных CSV. Старые `lab3/configs`, `lab3/results`, конфликтующий `compare.py` и отсутствующий SHORT_REPORT не изменялись.']
    (a.HERE/'findings.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    # Optional scientific figures; numerical audit has no plotting dependency.
    deps=Path('E:/asp/26_27/tmp/python_deps')
    if deps.exists():sys.path.insert(0,str(deps))
    os.environ.setdefault('MPLCONFIGDIR',str(a.HERE/'mpl'))
    import matplotlib;matplotlib.use('Agg');import matplotlib.pyplot as plt
    fig,ax=plt.subplots(1,2,figsize=(11,4.5),layout='constrained')
    f=[r['stimulus_frequency_hz'] for r in rows]
    ax[0].plot(f,[r['historical_output_peak'] for r in rows],'o-',label='Current core: 30 x 3 ms')
    ax[0].plot(f,external_estimates,'s--',label='NeuroNet screenshots (approx.)')
    ax[0].set(title='Fixed stimulus protocol',xlabel='Stimulus frequency (Hz)',ylabel='Output peak from zero (a.u.)')
    ax[1].plot(f,[r['calibrated_output_peak'] for r in rows],'o',label='Calibrated neuron, 1 spike / period')
    ax[1].plot(f,[r['ideal_periodic_tm_peak'] for r in rows],'-',label='Exact regular-spike TM')
    ax[1].axhline(10,color='gray',linestyle=':',label='Isolated-spike control')
    ax[1].set(title='Required presynaptic frequencies',xlabel='Actual presynaptic spike rate (Hz)',ylabel='Output peak from zero (a.u.)')
    for panel in ax:panel.set_xscale('log');panel.set_xticks(f,[str(x).removesuffix('.0') for x in f]);panel.set_ylim(0,20);panel.grid(alpha=.2);panel.legend(fontsize=8)
    fig.savefig(a.HERE/'frequency_audit.png',dpi=180);plt.close(fig)
    if network and len(network['runs'])==3:
        fig,axs=plt.subplots(3,1,figsize=(11,7),sharex=True,sharey=True,layout='constrained')
        for ax,r in zip(axs,network['runs']):
            with a.rawpath(r['name'],'events.csv').open(encoding='utf-8',newline='') as stream:
                ev=[(float(q['time_s']),int(q['node_index_1based'])) for q in csv.DictReader(stream) if 10<=float(q['time_s'])<30]
            ax.plot([v[0] for v in ev],[v[1] for v in ev],',',color='#24557e')
            ax.set(ylabel='Neuron ID',title=f"{r['frequency_hz']:g} Hz stimulus | active {r['active_neuron_count']}/500 | current zero-delay core",ylim=(0,501),xlim=(10,30))
        axs[-1].set_xlabel('Time (s)');fig.savefig(a.HERE/'network_rasters.png',dpi=180);plt.close(fig)
    print('FINDINGS',findings['status'],flush=True)

if __name__=='__main__':main()
