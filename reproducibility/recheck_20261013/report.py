"""Build the Russian comparison report from completed measurements only."""
import argparse, html, json, os, statistics, re, collections, platform
from datetime import datetime
from pathlib import Path
import numpy as np
import scipy, matplotlib
from replay import ROOT, read, save, sha, suff, load_events

HERE=Path(__file__).resolve().parent
def esc(x):return html.escape(str(x))
def fmt(x,d=8):return '—' if x is None else f'{x:.{d}g}' if isinstance(x,(int,float)) else esc(x)
def table(headers,rows):
    return '<div class="scroll"><table><thead><tr>'+''.join('<th>'+esc(x)+'</th>' for x in headers)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+fmt(x)+'</td>' for x in row)+'</tr>' for row in rows)+'</tbody></table></div>'

def interface_compare(out):
    runs=read(out/'interfaces/executions.json');results=[]
    for r in runs:
        prefix=out/r['group']/'raw'/r['name']
        files=('events.csv','trace.csv','weights.csv') if r['kind']=='checkpoint_build' else ('events.csv','trace.csv','weights.csv','deliveries.csv','pending_deliveries.csv','summary.json','experiment.json')
        matches={f:sha(suff(r['prefix'],f))==sha(suff(prefix,f)) for f in files}
        results.append(dict(kind=r['kind'],name=r['name'],byte_identical=matches))
        assert all(matches.values()),results[-1]
    save(out/'interfaces/comparison.json',results);return results

def make(out):
    a=read(out/'analysis.json');ind=read(out/'independent/results.json');prov=read(out/'provenance.json')
    burst=read(out/'burst_convergence/results.json');assert len(burst['rows'])==3
    assert burst['executable_sha256']==prov['executables']['network_studio_cli.exe']
    assert all(sha(p)==h for row in burst['rows'] for p,h in row.get('outputs_sha256',{}).items())
    reconciliation=read(out/'progress_reconciliation.json')
    assert reconciliation['verified_records']==241 and reconciliation['native_returncodes']==[0] and reconciliation['simulation_reruns']==0
    completed_at=datetime.now().astimezone().isoformat(timespec='seconds')
    interfaces=interface_compare(out);total=sum(g['completed'] for g in a['groups'].values())
    assert total==282,('Incomplete archive replay',total)
    expected=collections.Counter(j['group'] for j in read(out/'plan.json')+read(out/'extra_plan.json'));expected['lab2_bridge']=2
    assert dict(expected)=={g:v['completed'] for g,v in a['groups'].items()}
    assert len(ind['lab1'])==7 and len(ind['convergence'])==9 and len(interfaces)==10
    records=[read(p) for p in out.glob('*/runs/*.json')];assert all(r['complete'] for r in records)
    hashes={str(p):sha(p) for p in [ROOT/'build/Release/network_studio.exe',ROOT/'build/Release/network_studio_cli.exe',ROOT/'build/Release/network_studio_population.exe']}
    assert {Path(p).name:h for p,h in hashes.items()}==prov['executables']
    sources=read(out/'source_manifest.json');assert all(sha(ROOT/p)==h for p,h in sources['source_sha256'].items())
    environment=dict(os=platform.platform(),python=platform.python_version(),numpy=np.__version__,scipy=scipy.__version__,matplotlib=matplotlib.__version__)
    compiler_files=list((ROOT/'build/native/CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    assert len(compiler_files)==1
    compiler=compiler_files[0].read_text(encoding='utf-8')
    environment.update(compiler=re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]+)"',compiler).group(1),compiler_version=re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"',compiler).group(1))
    ctest=(out/'build_test_smoke.log').read_text(encoding='utf-8-sig');assert '100% tests passed, 0 tests failed out of 9' in ctest
    test_seconds=re.search(r'Total Test time \(real\) = ([\d.]+) sec',ctest).group(1)
    optimization=read(out/'optimization_comparison.json');exact=read(out/'optimization_archive_comparison.json')
    assert len(optimization['cases'])==25 and optimization['optimized_binaries']==prov['executables']
    for name in ('lab1_accuracy','lab2_populations','lab3_frequency','lab4_weight_changes'):assert (HERE/'figures'/(name+'.png')).is_file()
    integrity=read(out/'integrity.json');assert integrity['checked']==total and integrity['configurations_identical_after_explicit_migration']
    css='body{font:16px/1.55 system-ui,sans-serif;color:#203043;max-width:1120px;margin:40px auto;padding:0 24px;background:#fcfcfd}h1,h2{color:#123b57}h1{font-size:30px}h2{margin-top:42px}table{border-collapse:collapse;width:100%;font-size:14px;margin:16px 0}td,th{border:1px solid #d6e0e7;padding:8px 10px;text-align:right}td:first-child,th:first-child{text-align:left}th{background:#eaf2f7}tr:nth-child(even){background:#f5f8fb}.scroll{overflow:auto}img{width:100%;height:auto}code{background:#edf1f5;padding:2px 4px}a{color:#146899}.lead{font-size:19px;background:#e8f3ee;padding:20px;border-left:5px solid #198268}.note{background:#fff4df;padding:14px}small{color:#506477}'
    parts=['<!doctype html><html lang="ru"><meta charset="utf-8"><title>Повтор ЛР1–4 — Network Studio, 13.10.2026</title><style>'+css+'</style><body>',
        '<h1>Повтор лабораторных 1–4: что изменилось</h1><p>Серия начата 13 октября 2026 · отчёт сформирован '+esc(completed_at)+' · Network Studio · исходный коммит <code>'+prov['git_head']+'</code> · ветка <code>audit/labs-1-4-20261013</code></p>',
        '<p class="lead">Результаты после исправления интегратора изменились численно. Ниже отдельно сравниваются прежние расчёты Studio, новая сборка на тех же входах и наблюдения NeuroNet из прежних отчётов. Нулевая задержка сохранена во всех архивных опытах; параметры под внешние ответы не подбирались.</p>',
        '<p>Завершены <b>282 основных расчёта</b>: архивные сценарии и контроли, включая три полных опыта сети 500 нейронов из ЛР3, длинные популяционные опыты ЛР2 и два расчёта для сравнения исполнителей. Дополнительно выполнены 16 расчётов при третьем шаге, свежий независимый DOP853 для семи режимов ЛР1, сравнения GUI/CLI и предыдущей сборки. ЛР4 здесь — четвёртая выполненная работа; в методичке и каталоге <code>reproducibility/lab5</code> это ЛР5 (STDP).</p>',
        '<h2>Проверка приложения и происхождение результатов</h2><p>Штатная команда <code>powershell -ExecutionPolicy Bypass -File .\\build.ps1 -Test -Smoke</code>: <b>9/9 тестов</b>, '+test_seconds+' с под параллельной нагрузкой. GUI: 28 кадров, семь вкладок, без ошибки Direct3D. Дополнительные четыре лабораторные конфигурации в GUI дают все семь экспортов побайтово равными CLI.</p>',
        '<p>В шести лабораторных контролях предыдущая сборка checkpoint/resume и текущая сборка с задержками дают побайтово одинаковые события, состояния и веса (18 CSV). Это отделяет эффект ранее исправленного интегратора от добавления очереди задержек при <code>delay=0</code>. Новые поля журналов/схем не сравниваются как старые данные.</p>',
        table(['Серия','Завершено','Одинаковое число спайков по всем нейронам','Идентичные старые CSV'],[[g,v['completed'],v['same_spike_counts'],f"{v['csv_identical']}/{v['csv_compared']}"] for g,v in sorted(a['groups'].items())]),
        '<p>Побайтовое различие CSV само по себе не означает дефект: изменились локализация событий, момент сброса и внутренние участки интегрирования. Большая точечная разность напряжений около сдвинутого спайка не является мерой ошибки фазы.</p>',
        '<h2>ЛР1 — отдельный нейрон</h2>']
    parts.insert(-1,'<h2>Проверенное ускорение</h2><p>По согласованию с пользователем коэффициенты точных линейных переходов вычисляются один раз для одинаковых параметров, а корень порога ищется в независимой между событиями подсистеме нейрона. Прежний порядок суммирования входящих токов и оба полушага RK сохранены. Полное принятое состояние всё ещё вычисляет общий интегратор. При наличии астроцитов используется прежний полный поиск. Новые входящие списки являются производными данными и пересоздаются конструктором при восстановлении.</p><p>С сохранённой исходной сборкой выполнено '+str(len(optimization['cases']))+' побайтовых контролей: CSV, счётчики и полное двоичное состояние checkpoint (кроме идентификатора сборки и зависящей от него контрольной суммы). Для двух активных популяций TM/экспоненциальных синапсов сравниваются события и счётчики всех 500 нейронов, а также траектории 16 выбранных узлов (предел штатного экспорта). Дополнительно '+str(exact['cases'])+' завершённых лабораторных повторов исходной сборки дали '+str(exact['csv_files'])+' идентичных CSV с ускоренной версией. Штатный регрессионный тест проверяет ускоренную и полную локализацию для HH/Ижикевича, трёх моделей синапса, трёх правил STDP, задержек и шума, без допуска на различие состояния.</p>'+table(['Контроль','До, с','После, с','Отношение'],[[r['name'],r['wall_seconds']['original'],r['wall_seconds']['optimized'],r['speedup']] for r in optimization['cases'] if r['name'] in ('large_core','tm','tsodyks_markram_active','exponential_current_active')])+'<p>Это замеры при параллельно запущенных проверках, а не универсальная оценка производительности. Шаги, допуски и порядок событий не изменены. Строгий идентификатор совместимости checkpoint обновился: старые файлы нужно открывать исходной сборкой; защита не обходилась.</p>')
    parts.insert(-1,'<p class="note">Дополнительный стресс-контроль: при механической замене TM на экспоненциальную модель с прежним усилением 20 возникла лавина событий; запуск остановлен по длительности и не считается пройденным. Отдельный завершённый контроль экспоненциальной модели использует усиление 1. Это вспомогательные проверки оптимизации, не изменение архивных лабораторных протоколов.</p>')
    parts.insert(-1,'<p>Для сетей без астроцитов также исключены неиспользуемые линейные производные и повторные линейные переходы стадий RK. Нейронные стадии используют прежние выражения, а синаптические координаты получают те же точные значения: общий линейный результат используется для стадий 2/3 и для стадии 4/итоговой комбинации. Норма ошибки, полное состояние и адаптивные решения не меняются. Граница нелинейной части пересоздаётся конструктором и не является новым состоянием checkpoint. Эта версия повторно прошла все 25 побитовых контролей.</p>')
    names=['01_rest','02_single_pulse','03_autonomous','04_periodic_02hz','04_periodic_10hz','04_periodic_20hz','05_dc_4','02_reference_30x3ms']
    parts.append(table(['Опыт','Спайки до → после','Частота по ISI до, Гц','После, Гц'],[[n,f"{a['lab1'][n]['old'][0]['spikes']} → {a['lab1'][n]['new'][0]['spikes']}",a['lab1'][n]['old'][0]['isi_rate_hz'],a['lab1'][n]['new'][0]['isi_rate_hz']] for n in names]))
    user_names=['user_dc3p8_12p5us','user_pulse_2hz','04_reference_10hz_30x3ms','user_pulse_20hz']
    parts.append('<p>Отдельно повторены сохранённые контроли с параметрами из пользовательского отчёта: постоянный уровень 3,8 и импульсы 30×3 мс. Автоколебательный контроль выше использует именно b=0,27.</p>'+table(['Контроль исходных параметров','Спайки до → после','Частота по ISI до, Гц','После, Гц'],[[n,f"{a['lab1'][n]['old'][0]['spikes']} → {a['lab1'][n]['new'][0]['spikes']}",a['lab1'][n]['old'][0]['isi_rate_hz'],a['lab1'][n]['new'][0]['isi_rate_hz']] for n in user_names]))
    parts.append('<p>При 3,8 устойчивый ритм сохранился: 29 спайков в обоих расчётах. При импульсах 30×3 мс Studio по-прежнему может давать несколько спайков на стимул. Поэтому ответы с частотой стимулятора в исходном отчёте NeuroNet не следует объявлять точно воспроизведёнными на основании коротких односпайковых контролей.</p>')
    changes=[(n,v) for n,v in a['lab1'].items() if not v['event_comparison']['counts_equal']]
    parts.append('<p>Изменения количества событий в проверенных шумовых/дополнительных опытах:</p>'+table(['Опыт','До, по нейронам','После'],[[n,str(v['event_comparison']['old_per_node']),str(v['event_comparison']['new_per_node'])] for n,v in changes]))
    parts.append('<p>Свежий независимый решатель DOP853: точные границы входа, терминальные корни порога и сброс состояния в момент события; миллисекунды, rtol=1e−11, atol=1e−12, max_step=0,5 мс. Максимальная ошибка времени спайка:</p>')
    parts.append(table(['Опыт','Архив 25 мкс, мс','Новый 25 мкс, мс','12,5 мкс, мс','6,25 мкс, мс'],[[n]+[v[k]['max_event_error_ms'] for k in ('archive_25us','current_25us','current_12p5us','current_6p25us')] for n,v in ind['lab1'].items()]))
    parts.append('<img src="figures/lab1_accuracy.png" alt="Ошибка времён спайков"><p>Количество событий совпало с DOP853 во всех семи режимах. На уровне порядка 10⁻⁷ мс и ниже таблица уже отражает погрешность эталона и округления; строгую монотонность последних цифр не следует трактовать как критерий сходимости.</p>')
    noise=[]
    for sigma in ('4p0','4p5'):
        selected=[a['lab1'][f'user_noise_global_{sigma}_seed{s}'] for s in (11,23,42,71,101)]
        vals={which:[n['rate_hz'] for r in selected for n in r[which]] for which in ('old','new')}
        noise.append([sigma.replace('p',','),*[f'{statistics.mean(vals[k]):.6f} ± {statistics.stdev(vals[k]):.6f}' for k in ('old','new')]])
    parts.append(table(['Шум σ','До: частота ± SD, Гц','После: частота ± SD, Гц'],noise))
    parts.append('<p>Окно 1–10 с, два нейрона × пять seed. Это одна и та же реализация шума Studio; генератор NeuroNet неизвестен. Исходный импульс 30×3 мс по-прежнему может вызывать несколько спайков: он не эквивалентен подобранному стимулу 30×1 мс.</p><h2>ЛР2 — кольца и популяции</h2>')
    ring_names=['tm_n03_w1','tm_n05_w1','tm_n10_w1','tm_n11_w1','tm_n10_w0p7','tm_n10_w0p5','exp_n10_w0p7']
    parts.append(table(['Кольцо','Циркуляция до / после','Спайки до / после','Частота поздних ISI до / после'],[[n,f"{a['rings'][n]['old']['sustained']} / {a['rings'][n]['new']['sustained']}",f"{a['rings'][n]['old']['spikes']} / {a['rings'][n]['new']['spikes']}",f"{fmt(a['rings'][n]['old']['isi_rate_hz'])} / {fmt(a['rings'][n]['new']['isi_rate_hz'])}"] for n in ring_names]))
    changed=[n for n,v in a['rings'].items() if v['old']['sustained']!=v['new']['sustained']]
    parts.append('<p>Изменившиеся классификации циркуляции по прежнему критерию (не менее двух событий на каждом узле в 4–5 с, порядок ≥95%, последний спайк ≥4,9 с): '+(esc(', '.join(changed)) if changed else '<b>нет</b>')+'. Наличие циркуляции в пятисекундном окне не доказывает её бесконечной устойчивости.</p>')
    parts.append('<p>Расхождение с исходным отчётом NeuroNet сохраняется: там кольцо из 10 нейронов поддерживало активность при весах 1, 0,7 и 0,5; в исходной TM-модели Studio эти кольца затухают. Исправление численной схемы само по себе это различие моделей не устранило. Экспоненциальный синапс — отдельный контроль, а не доказанная реконструкция NeuroNet.</p>')
    poprows=[];popstats={}
    for k in (15,30):
        ss=[a['populations'][f'k{k}_seed{s}'] for s in (11,42,101)];popstats[k]={}
        for which in ('old','new'):
            popstats[k][which]={key:[r[which][key] for r in ss] for key in ('mean_rate_hz','burst_rate_hz')}
        poprows.append([f'{k} входов',*[f'{statistics.mean(popstats[k][w][key]):.6f} ± {statistics.stdev(popstats[k][w][key]):.6f}' for key in ('mean_rate_hz','burst_rate_hz') for w in ('old','new')]])
    parts.append(table(['Сеть','Спайки до, Гц','После, Гц','Пачки до, Гц','После, Гц'],poprows)+'<img src="figures/lab2_populations.png" alt="Популяционная статистика">')
    parts.append('<p>Среднее ± SD по seed 11/42/101, окно 5–30 с, 500 нейронов, σ=3. Пачки определены прежним независимым алгоритмом (50-мс популяционная частота, максимум &gt;5 Гц, расстояние пиков 50 мс). При изменении отдельных траекторий следует сравнивать и статистику, а не требовать совпадения каждого шумового спайка.</p>')
    parts.append(table(['Исходный граф NeuroNet','Частота до / после, Гц','Пачки до / после, Гц'],[[n,f"{a['populations'][n]['old']['mean_rate_hz']:.6f} / {a['populations'][n]['new']['mean_rate_hz']:.6f}",f"{a['populations'][n]['old']['burst_rate_hz']:.6f} / {a['populations'][n]['new']['burst_rate_hz']:.6f}"] for n in ('nn_k15','nn_k30')]))
    parts.append('<p>Два импортированных графа проверены отдельно от вновь сгенерированных популяций: в них фактически 5577 и 11891 связей. Номинальные подписи «15» и «30» не означают, что каждый нейрон имеет ровно такое число входов; это нельзя смешивать с контролями по сгенерированным графам.</p>')
    parts.append('<p>Качественное наличие пачечной активности воспроизводится. Точное совпадение частоты пачек с NeuroNet не установлено: в ручном отчёте сохранены показания счётчика 29 и 32 к моменту 29 с, тогда как здесь частота рассчитана по журналам собственным явно заданным детектором в окне 5–30 с. Эти оценки нельзя считать одной и той же измерительной процедурой.</p>')
    bridge=read(out/'bridge_comparison.json');parts.append('<p>Контроль основного ядра GUI против агрегированного исполнителя: 500 нейронов, 0,2 с, '+str(bridge['spikes'])+' спайков, количество по каждому нейрону совпало; max |Δt|='+fmt(bridge['max_event_difference_s'])+' с, max |ΔV|='+fmt(bridge['max_voltage_difference_mV'])+' мВ. Длинные популяционные серии рассчитаны штатным агрегированным исполнителем, который не поддерживает ненулевые задержки.</p>')
    parts.append('<h2>ЛР3 — кратковременная пластичность</h2>')
    freqs=[.1,.5,1,5,10,25,50,100];external=[14.5,14.5,14.5,17.5,15.5,10,6.75,6.75];tmrows=[]
    for f,ext in zip(freqs,external):
        n=f'f{f:g}'.replace('.','p');r=a['lab3'][n]
        tmrows.append([f,ext,r['old']['peak_mean'],r['new']['peak_mean'],r['old']['spike_rate_hz'],r['new']['spike_rate_hz']])
    parts.append(table(['Стимулы, Гц','NeuroNet ≈ пик','Studio до','Studio после','Спайки/с до','После'],tmrows)+'<img src="figures/lab3_frequency.png" alt="Частотный ответ">')
    max_tm=max(max(r['new'].get('tm_max_abs_errors',{'x':0}).values()) for r in a['lab3'].values())
    parts.append('<p>Пики рассчитаны аналитической TM-рекуррентностью по фактическим новым пресинаптическим событиям, включая остаточный хвост; максимальная ошибка записанных ресурсов u/x/y относительно независимого точного перехода — '+fmt(max_tm)+'. Калиброванные односпайковые протоколы также повторены; их настройки не подбирались заново.</p>')
    parts.append(table(['Полная сеть, частота','Активных нейронов 10–30 с: до / после','Спайков в окне: до / после','Средняя частота до / после, Гц'],[[n,f"{r['old']['active_neurons']} / {r['new']['active_neurons']}",f"{r['old']['steady_spikes']} / {r['new']['steady_spikes']}",f"{r['old']['mean_rate_hz']:.6f} / {r['new']['mean_rate_hz']:.6f}"] for n,r in a['lab3_network'].items()]))
    parts.append('<p class="note">В исходном ручном опыте NeuroNet частоту меняли последовательно без сброса сети. Здесь повторены сохранённые протоколы Studio: отдельные запуски по 30 с с одинаковым начальным состоянием. Это позволяет сравнить версии Studio, но не воспроизводит событийную историю того непрерывного ручного сеанса.</p>')
    sparse=a['lab3_network']['network_net1_0p1Hz']
    parts.append('<p>Изменение полной сети при 0,1 Гц существенно: в окне 10–30 с число событий выросло с '+str(sparse['old']['steady_spikes'])+' до '+str(sparse['new']['steady_spikes'])+
        ' ('+fmt(100*(sparse['new']['steady_spikes']/sparse['old']['steady_spikes']-1))+'%). Поэтому оценка «изменились лишь последние цифры» для всей серии неверна. Отдельно проверен полный первый всплеск до 0,5 с при уменьшении шага; результат приведён ниже.</p>')
    parts.append('<h2>ЛР4 — долговременная пластичность (ЛР5 методички)</h2>')
    names=sorted(n for n in a['lab4'] if n.endswith('_dt25us') and not n.startswith('width1') and a['lab4'][n]['group']=='lab4_updated')
    wrows=[]
    from analyze import stdp
    for n in names:
        r=a['lab4'][n];old,new=r['old_weights'][0],r['new_weights'][0];ext=stdp.EXTERNAL.get(n.removesuffix('_dt25us'))
        wrows.append([n.removesuffix('_dt25us'),ext,old,new,new-old,100*(new/old-1) if old else None,f"{r['old_spikes']} → {r['new_spikes']}"])
    parts.append(table(['Протокол, 100 с','NeuroNet','Studio до','После','Δw','Δw, %','Спайки пре/пост'],wrows)+'<img src="figures/lab4_weight_changes.png" alt="Изменение весов STDP">')
    pairs=[(a['lab4'][n],a['lab4'][n.replace('_dt25us','_dt12p5us')]) for n in names]
    assert len(pairs)==14
    parts.append('<p>При уменьшении текущего шага с 25 до 12,5 мкс число спайков совпало в '+str(sum(x['new_spikes']==y['new_spikes'] for x,y in pairs))+
        ' из 14 основных протоколов; максимальное изменение конечного веса — '+fmt(max(abs(x['new_weights'][0]-y['new_weights'][0]) for x,y in pairs))+'.</p>')
    parts.append('<p>Все веса независимо пересчитаны по полным журналам событий прямыми суммами прошлых спайков, без использования внутренних следов C++. Максимальное расхождение по всей записанной истории — '+fmt(max(max(r['replay_errors'],default=0) for r in a['lab4'].values()))+'. Одновременные события не образуют взаимной пары; медленный постслед в триплетной LTP берётся до текущего спайка.</p>')
    dep=a['lab4']['t1_post_pre_10Hz_dt25us'];dw=dep['new_weights'][0]-dep['old_weights'][0]
    parts.append('<p>В протоколе «пост–пре, 10 Гц» относительное изменение заметно ('+fmt(100*(dep['new_weights'][0]/dep['old_weights'][0]-1))+'%), но абсолютное составляет '+fmt(dw)+'. Качественный результат — сильная депрессия — сохраняется. Поэтому относительные изменения очень малых конечных весов нужно читать вместе с самими весами и абсолютной разностью.</p>')
    dep_fine=read(out/'lab4_updated/runs/t1_post_pre_10Hz_dt12p5us.json')
    dep_quarter=ind['convergence']['lab4_updated/t1_post_pre_10Hz_dt25us']
    parts.append('<p>Для этого же опыта прежний вес при уменьшении шага до 12,5 мкс составлял '+fmt(dep_fine['old_summary']['final_synapse_weights'][0],13)+'. Новые значения при 25 / 12,5 / 6,25 мкс: '+
        ' / '.join(fmt(w,13) for w in (dep['new_weights'][0],dep_fine['summary']['final_synapse_weights'][0],dep_quarter['weights'][0]))+
        '. Уточнение шага подтверждает устойчивость нового результата.</p>')
    pathrows=[]
    for topology in ('two','chain','shortcut'):
        rows=[a['lab4'][f'{topology}_seed{s:02}_noise3_dt25us'] for s in range(10)]
        for edge in range(len(rows[0]['old_weights'])):
            pathrows.append([topology,f'S{edge+1}',statistics.mean(r['old_weights'][edge] for r in rows),statistics.mean(r['new_weights'][edge] for r in rows)])
    parts.append(table(['Топология, 5 с','Связь','Средний вес до','После'],pathrows))
    parts.append('<p>Сетевые протоколы повторены по десяти одинаковым seed; отдельно сохранены 30 проверок длительностей 6,1 и 12,525 с. Рост прямой связи сам по себе не подтверждает правило кратчайшего пути: нужно также устойчивое подавление обходного пути. Модель задержек NeuroNet не восстановлена.</p><h2>Уточнение шага</h2>')
    convrows=[]
    for key,r in ind['convergence'].items():
        group,name=key.split('/');base=read(out/group/'runs'/(name+'.json'))
        fine_name=name.replace('_dt25us','_dt12p5us') if group=='lab4_updated' else name+'_fine' if group=='lab3' else name+'_dt12p5us'
        fine=read(out/group/'runs'/(fine_name+'.json'))
        convrows.append([key,str(base['summary']['neuron_spikes']),str(fine['summary']['neuron_spikes']),str(r['spikes']),
            f"{base['summary']['final_synapse_weights'][0]:.12g} / {fine['summary']['final_synapse_weights'][0]:.12g} / {r['weights'][0]:.12g}"])
    parts.append(table(['Опыт','Спайки 25 мкс','12,5 мкс','6,25 мкс','Первый вес: 25 / 12,5 / 6,25'],convrows))
    nc=a['large_network_convergence'];assert len(nc)==5
    parts.append('<p>Отдельный контроль связанной сети из 500 нейронов, 0,2 с: сравнение с самым мелким шагом 1,5625 мкс. Это проверка внутренней сходимости, а не независимый эталон большой сети. RMS напряжения включает участки около спайков.</p>'+table(['Шаг, мкс','Спайков','Число по нейронам совпадает','max |Δt|, мс','RMS ΔV, мВ'],[[r['dt_us'],r['spikes'],r['same_per_neuron_counts_as_finest'],r['max_event_difference_ms'],r['recorded_voltage_rms_difference_mV']] for r in nc]))
    parts.append('<p>Дополнительные два расчёта после обнаружения большого изменения при 0,1 Гц: весь первый сетевой всплеск, окно [0; 0,5) с. Базовый шаг взят из полного завершённого опыта; эталон этой таблицы — собственный расчёт при 6,25 мкс. В архиве первый всплеск содержал 1620 событий и заканчивался в 0,257525 с.</p>'+table(['Шаг, мкс','Спайков','Последний спайк, с','Число по нейронам совпадает','max |Δt|, мс','RMS ΔV, мВ'],[[r['dt_us'],r['spikes'],r['last_spike_s'],r['same_per_neuron_counts_as_finest'],r['max_event_difference_ms'],r['recorded_voltage_rms_difference_mV']] for r in burst['rows']]))
    parts.append('<p>На всех трёх шагах получено 2495 спайков; количество совпадает отдельно для каждого нейрона. Удлинение первого всплеска относительно архива сохраняется при уточнении шага. Ошибка времени относительно 6,25 мкс уменьшается с 0,01837 до 0,004308 мс, RMS напряжения — с 0,0002418 до 0,00006397 мВ. Это существенный динамический эффект исправления событийной траектории, а не только новый формат времён в CSV.</p>')
    parts.append('<p>Эти проверки охватывают первый всплеск. Полные 30-секундные сетевые траектории не повторялись на всех уменьшенных шагах и не сравнивались с независимым адаптивным решателем всей сети; ограничение не следует переносить на проверенные отдельные нейроны, TM-рекуррентности и пересчёт STDP.</p>')
    parts.append('<h2>Ограничения и воспроизведение</h2><p>Архивы и исходные пользовательские результаты не перезаписывались. Полные CSV и журналы находятся в <code>'+esc(str(out.relative_to(ROOT)))+'</code>; рядом с этим отчётом сохранены компактные JSON и скрипты. Длительные первые запуски исходной и промежуточной сборки остановлены при подготовке ускорения; их каталоги сохранены отдельно. В итоговую сводку входят только полностью завершённые запуски окончательной сборки. Начальный анализ ЛР1 был исправлен: в старом архиве нет отдельного CSV весов. Успешные нативные расчёты повторно разобраны без подмены данных; это ошибка обвязки сравнения, не сбой симулятора.</p><p>Исторический локальный <code>lab3/compare.py</code> содержит маркеры конфликта; он не исполнялся и не изменялся. Протоколы взяты из сохранённых JSON, TM проверена отдельным <code>lab3/reference_tm.py</code>. Для повторения нужен полный локальный архив, а также Python с NumPy/SciPy/Matplotlib; одного клона Git недостаточно для старых больших CSV.</p><p>Прохождение программных и численных проверок не доказывает биологическую достоверность. Сравнение с NeuroNet ограничено неизвестными соглашениями проведения, шума, интегратора, стартового состояния и продолжительностью ручных сеансов. В этих повторах новые задержки намеренно равны нулю; произвольное добавление задержек изменило бы эксперимент.</p>')
    parts.append('<pre>powershell -ExecutionPolicy Bypass -File .\\build.ps1 -Test -Smoke\npython -B reproducibility/recheck_20261013/replay.py --output verification/labs_new --workers 4\npython -B reproducibility/recheck_20261013/extras.py --output verification/labs_new\npython -B reproducibility/recheck_20261013/bridge.py --output verification/labs_new\npython -B reproducibility/recheck_20261013/independent.py --output verification/labs_new\npython -B reproducibility/recheck_20261013/interface_controls.py --output verification/labs_new\npython -B reproducibility/recheck_20261013/analyze.py --output verification/labs_new</pre>')
    parts.append('<p>При ведении журнала Windows временно запретила атомарную замену вспомогательного <code>progress.json</code> (WinError 5). Диспетчер закончил ожидание уже запущенных работ и сообщил эту ошибку; все 241 нативных расчёта основной очереди независимо завершились с кодом 0 и полными записями. Счётчик восстановлен после повторной проверки входов и всех хэшей выходных файлов, без перезапуска симуляций. Исходный журнал сохранён; в Python-обвязку добавлен ограниченный повтор атомарного переименования. Отдельная итоговая проверка охватывает все 282 основных расчёта.</p>')
    parts.append(table(['Новая сборка','SHA-256'],[[p,h] for p,h in hashes.items()]))
    parts.append('<p>Среда проверки: '+esc('; '.join(k+' '+v for k,v in environment.items()))+'.</p>')
    parts.append('<p><a href="analysis.json">Полные сравнительные метрики</a> · <a href="independent_results.json">Независимый эталон и третий шаг</a> · <a href="manifest.json">Манифест</a></p></body></html>')
    (HERE/'REPORT_RU.html').write_text('\n'.join(parts),encoding='utf-8')
    save(HERE/'analysis.json',a);save(HERE/'independent_results.json',ind);save(HERE/'burst_convergence_results.json',burst)
    save(HERE/'manifest.json',dict(base_commit=prov['git_head'],completed_at=completed_at,archive_runs=total,extra_dt_runs=18,ctest='9/9',
        binary_sha256=hashes,raw_directory=str(out),interfaces=interfaces,bridge=bridge,optimization=optimization,optimization_archive=exact,integrity=integrity,source_manifest=sources,
        environment=environment,burst_convergence=burst,progress_reconciliation=reconciliation,report_sha256=sha(HERE/'REPORT_RU.html'),script_sha256={p.name:sha(p) for p in HERE.glob('*.py')}))
    print('REPORT READY',HERE/'REPORT_RU.html',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    make(ap.parse_args().output.resolve())
