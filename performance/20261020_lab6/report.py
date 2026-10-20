"""Assemble the report from actual completed artifacts; fail if evidence is absent."""
import json,statistics,hashlib,re
from pathlib import Path
p=Path(__file__).resolve().parent;root=p.parents[1]
def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def save(name,value):p.joinpath(name).write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf-8')
def med(rows,key):return statistics.median(r[key] for r in rows)
def fmt(rows,key):
 v=[r[key] for r in rows];return f'{statistics.median(v):.4f} [{min(v):.4f}–{max(v):.4f}]'
base=read(p/'measurements/baseline/summary.json');after=read(p/'measurements/optimized/summary.json')
old=read(p/'measurements/baseline/measurements.json');new=read(p/'measurements/optimized/measurements.json')
over=read(p/'validation/overheads/results.json');validation=read(p/'validation/equivalence/result.json');tests=read(p/'test-duration.json')
assert tests['exit_code']==0 and validation['passed'] and len(over['results'])==21
long=read(p/'validation/paired/long.json');assert long['passed'];save('long-control.json',long)
paired=read(p/'validation/paired/results.json');assert len(paired['results'])==12;save('paired-timings.json',paired)
log=(p/'full-tests.log').read_text(encoding='utf-8-sig');test_count=int(re.search(r'tests failed out of (\d+)',log).group(1));test_seconds=float(re.search(r'Total Test time \(real\) =\s*([0-9.]+)',log).group(1))
save('optimized-timings.json',after);save('raw-timings.json',{'baseline':old,'optimized':new});save('overheads.json',over)
save('equivalence.json',{k:v for k,v in validation.items() if k!='details'})
profiles={}
for version,folder in [('baseline',p/'pilot'),('optimized',p/'validation/profiles')]:
 profiles[version]={}
 for name in ['profile1-quiet','profile1-burst','profile8-quiet','profile8-burst','profile8-long']:
  profiles[version][name]=read(folder/name/'profile.json')
save('profiles.json',profiles)
files=['network_studio.exe','network_studio_cli.exe','network_studio_lab_benchmark.exe','network_studio_runner_benchmark.exe']
manifest={'binaries':{name:{'path':str(root/'build/Release'/name),'sha256':hashlib.sha256((root/'build/Release'/name).read_bytes()).hexdigest()} for name in files},'checkpoint_build':re.search(r'"(.*?)"',(root/'build/native/checkpoint_build.h').read_text()).group(1),'algorithm':'event-rk4-doubling-exact-linear-localized-stimulators-v4','checkpoint_schema':5,'branch':'perf/lab6-20261020'}
save('build-manifest.json',manifest)
lines=['# ЛР6: профиль и оптимизация, 20.10.2026','',
'Исходная сеть — **500 Izhikevich, 15 041 TM, 12 009 STDP, σ=3, seed=42, dt=25 мкс**. Исходный JSON не изменён; ранее накопленные изменения сохранены. Все входные SHA проверены повторно. Аппаратура: Intel Core i9-11900KF; фактические режимы показаны ниже. Fast-math не включён.',
'', '## Методика и реально выполненный объём','',
'3 последовательных повтора для каждого режима, 2 естественных окна исходного опыта: 0–50 мс (0 спайков), 50–100 мс (4 977 спайков, 150 532 доставки). Второе окно загружается из совместимого checkpoint на 50 мс. Длительность Config остаётся 150 с; остановка происходит на безопасной границе. Загрузка, CSV, анализ и checkpoint исключены из solve_seconds. Порядок потоков в повторах циклически меняется. Прогоны не запускались одновременно со сборкой или другими измерениями.',
'', 'Дополнительные проверки записи доходят до **0,5 модельной секунды**, также в 3 повторах. Это не повтор лабораторного этапа 150 с или ветвей до 400 с. Старый лабораторный checkpoint отвергнут исходным штатным загрузчиком по схеме. Его совместимость не обходилась. Экстраполяция длительного опыта здесь не выдаётся за измерение.',
'',f'Время сценариев целиком (с загрузкой/выгрузкой): эталонная матрица **{base["wall_seconds"]:.2f} с**, новая **{after["wall_seconds"]:.2f} с**; проверки эквивалентности **{validation["wall_seconds"]:.2f} с**; запись/интерфейс **{over["wall_seconds"]:.2f} с**. Полная штатная сборка/тесты/smoke: **{tests["wall_seconds"]:.2f} с**, код завершения {tests["exit_code"]}. Отдельно диагностическое профилирование и контрольные прогоны указаны в исходных timing.json; эти wall-интервалы не являются модельным временем.',
'', '## Время расчёта','', 'Секунды wall: **медиана [минимум–максимум]**, 3 повтора. Координатор включён в число вычислительных потоков.','',
'| Режим (факт) | 0–50 мс: до | 0–50 мс: после | Ускорение | 50–100 мс: до | 50–100 мс: после | Ускорение |',
'|---|---:|---:|---:|---:|---:|---:|']
for mode in [1,2,4,6,8,12,16,0]:
 groups=[[r for r in rows if r['window']==w and r['requested_threads']==mode] for w in ['quiet','burst'] for rows in [old,new]]
 oq,nq,ob,nb=groups;label='Auto' if mode==0 else str(mode);actual=sorted({r['effective_threads'] for r in nq+nb})
 lines.append(f'| {label} ({"/".join(map(str,actual))}) | {fmt(oq,"solve_seconds")} | {fmt(nq,"solve_seconds")} | {med(oq,"solve_seconds")/med(nq,"solve_seconds"):.2f}× | {fmt(ob,"solve_seconds")} | {fmt(nb,"solve_seconds")} | {med(ob,"solve_seconds")/med(nb,"solve_seconds"):.2f}× |')
lines+=['','Чередующаяся контрольная серия AB/BA (50–100 мс, ещё 3 повтора каждой сборки):','', '| Потоки | Исходная, с [min–max] | Новая, с [min–max] | Отношение медиан |','|---|---:|---:|---:|']
for mode in [8,16]:
 a=[r for r in paired['results'] if r['requested_threads']==mode and r['version']=='baseline'];b=[r for r in paired['results'] if r['requested_threads']==mode and r['version']=='optimized']
 lines.append(f'| {mode} | {fmt(a,"solve_seconds")} | {fmt(b,"solve_seconds")} | {med(a,"solve_seconds")/med(b,"solve_seconds"):.3f}× |')
lines+=['',f'Чередующаяся серия заняла {paired["wall_seconds"]:.2f} с вместе с загрузкой/экспортом. В основной серии медиана 16 потоков ухудшилась, но чередующаяся серия даёт почти одинаковые времена. Это большой разброс, а не подтверждённое устойчивое ускорение или однозначный эффект регрессии. Новый последовательный проход подготовки токов, барьеры и аппаратная конкуренция могут ограничивать этот режим; аппаратные счётчики cache miss/stall не измерялись. Его преимущества над 8 не подтверждены.','', '**Auto оставлен прежним**: для ≥8192 TM-связей — до 8 потоков, с аппаратным ограничением; для 4096–8191 — до 6, 2048–4095 — до 4, малые/неподдерживаемые модели последовательно. Короткие ранние окна не доказывают оптимальность другого общего правила. 12/16 доступны вручную; увеличение числа потоков не гарантирует ускорения. Вариации Auto и ручных 8 отражают отдельные запуски одинакового фактического режима.','',
'## Где затрачивается время','',
'Один диагностический профиль каждого участка; **exclusive wall**, без двойного учёта вложенных фаз. Профилирующая сборка содержит таймеры и имеет другой checkpoint ID; её времена не используются для расчёта ускорения Release. В параллельном RHS нейроны и токи исполняются в одной фазе: разделённые значения ниже относятся к одному потоку.','',
'| Фаза, 1 поток | Спокойный: до → после, с | Всплеск: до → после, с |', '|---|---:|---:|']
for key,title in [('rhs_neurons','Производные нейронов'),('rhs_synapses','Суммирование синаптических токов'),('exact_synapses','Точные переходы синапсов'),('trial','Норма ошибки и остальные RK-циклы'),('stdp','STDP'),('thresholds','Поиск событий без вложенных проекций'),('threshold_sample','Проекции для локализации спайков'),('event_jumps','Сбросы, доставки и прочие операции группы'),('noise','Генерация новых значений шума'),('external','Подготовка входа и чтение кэша шума'),('journals','Журналы в памяти и запись отсчётов'),('bounds','Проверка допустимости состояния'),('step','Прочее на шаге, включая подготовку представлений')]:
 cols=[]
 for window in ['quiet','burst']:
  vals=[]
  for version in ['baseline','optimized']:
   cats={c['name']:c for c in profiles[version]['profile1-'+window]['categories']};vals.append(cats[key]['exclusive_seconds'])
  cols.append(f'{vals[0]:.5f} → {vals[1]:.5f}')
 lines.append(f'| {title} | '+ ' | '.join(cols)+' |')
lines+=['','Профиль продолжения 0,1–0,5 с, 8 потоков (exclusive wall):','', '| Фаза | До, с | После, с |', '|---|---:|---:|']
for key in ['rhs_synapses','trial','exact_synapses','stdp','bounds','step','journals','external','thresholds']:
 vals=[next(c['exclusive_seconds'] for c in profiles[v]['profile8-long']['categories'] if c['name']==key) for v in ['baseline','optimized']]
 lines.append(f'| {key} | {vals[0]:.4f} | {vals[1]:.4f} |')
lines+=['', 'Изменены два подтверждённых участка: компактные `weight*gain` перед каждым внутренним участком (повторное чтение разрозненных весов удалено из RK-стадий), и SSE2-норма ошибки по двум независимым координатам. Порядок слагаемых, умножений, параметры, события и обновления обучения сохранены. Все координаты нормы и все синапсы по-прежнему вычисляются. Дополнительный буфер — 16 байт на активную связь, около 235 KiB в этой сети. После каждого STDP следующий участок получает актуальные веса.', '',
'## Запись, снимки, GUI и анализ','',
'Для пары rolling/archive одинаковая память на 10 000 отсчётов; на коротких участках она не заполняется. Archive дополнительно пишет доставки. Параметры сети не изменены. Время flush отдельно от solve; автоматические коммиты во время шага входят в solve. Пиковый Working Set до экспорта включает также загрузку JSON; это не обещание постоянного RSS на 400 с.','',
'| До модельного времени | Режим | Расчёт, с [min–max] | Создание архива, с | Flush, с | Анализ RAM, с | Анализ архива с чтением, с | Пик до экспорта, MiB |', '|---|---|---:|---:|---:|---:|---:|---:|']
for stop in [.1,.5]:
 for mode in ['rolling','archive']:
  rows=[r for r in over['results'] if r['mode']==mode and r['model_seconds']==stop]
  lines.append(f'| {stop} с | {mode} | {fmt(rows,"solve_seconds")} | {med(rows,"warmup_seconds"):.4f} | {med(rows,"archive_flush_seconds"):.4f} | {med(rows,"analysis_seconds"):.6f} | {med(rows,"archive_analysis_seconds"):.4f} | {med(rows,"solve_peak_working_set_bytes")/1048576:.1f} |')
lines+=['','Для всех повторов запись не изменила бинарные научные числа/CSV, а результаты анализа памяти и архива совпали. **Native GUI — скрытое окно Win32/DX11**, стандартные встроенные графики, без теста видимой анимации/других мониторов. CPU-рендер не равен времени GPU. Вычислитель и GUI работают одновременно: их времена нельзя складывать. Present включает ожидание вывода; подробные счётчики Present/кадров сохранены в overheads.json.','',
'| Потребитель, 0–0,1 с | Wall, с | Вычислитель, с | Подготовка снимков, с | Применение / App draw, с | Рендер CPU, с |', '|---|---:|---:|---:|---:|---:|']
for mode,hz in [('runner',10),('runner',20),('gui',10)]:
 rows=[r for r in over['results'] if r['mode']==mode and r['display_hz']==hz]
 draw='consumer_apply_seconds' if mode=='runner' else 'app_draw_snapshot_seconds'
 render=0 if mode=='runner' else med(rows,'render_cpu_seconds')
 lines.append(f'| {mode}, {hz} Гц | {fmt(rows,"wall_seconds")} | {med(rows,"worker_compute_seconds"):.4f} | {med(rows,"worker_publish_seconds"):.4f} | {med(rows,draw):.4f} | {render:.4f} |')
lines+=['','Все checkpoint интерфейса/потребителей совпали с прямым расчётом в той же сборке. CSV, checkpoint и загрузка измерены отдельно в `raw-timings.json`; анализ в памяти измерен отдельно от его экспорта.','',
'## Корректность и совместимость','',f'- {validation["checks"]} проверок сравнительного сценария: полные payload NSCP для всех режимов/повторов, бинарные состояния и все экспорты, перезапуск CLI с 8→12 потоками в каждой совместимой сборке. Отдельно эталонные сценарии задержек, triplet, HH/астроцита и общих/пространственных стимулов.',
f'- Полная штатная серия: **{test_count}/{test_count}**, **{test_seconds:.2f} с**; затем native smoke. См. `full-tests.log` и `validation/native_smoke`. Тест нормы сравнивает прежнюю скалярную формулу побитово на произвольных binary64, специальных значениях и неравных блоках; тест packed kernel сравнивает траекторию с общим вычислительным путём.',
'- Схема NSCP **5** и алгоритм **v4** сохранены, но build ID изменился. Старый файл не загружается новой сборкой; контроль не обходился. Межсборочное сравнение исключает только build ID и зависимую checksum, проверяя исходную целостность. Само по себе оно не разрешает межсборочное продолжение.',
'- 150–400 модельных секунд, поздняя стационарная активность, заполненные ограниченные журналы и несколько одновременно видимых окон не проверялись этой серией. Численное совпадение программ не доказывает биологической достоверности модели.',
'', '## Готовая сборка и воспроизведение','',f'Checkpoint build ID: `{manifest["checkpoint_build"]}`.','']
for name,item in manifest['binaries'].items():lines += [f'- `{item["path"]}` — SHA-256 `{item["sha256"]}`.']
lines+=['','Команды и соглашения: [README_RU.md](README_RU.md). Исходные EXE/снимок дерева сохранены локально; старые результаты не перезаписаны. Измерения, проверка и отчёты находятся в этом новом каталоге.']

# Keep the longer single control distinct from the replicated short windows.
i=lines.index('## Где затрачивается время')
more=['## Более длинное продолжение и отдельные расходы ввода/вывода','',
 f'Продолжение с 0,1 до **0,5 с**, 8 потоков: до **{long["results"][0]["solve_seconds"]:.3f} с**, после **{long["results"][1]["solve_seconds"]:.3f} с** wall. Это **один контроль**, не медиана. Получены **{long["results"][1]["events"]} спайков и {long["results"][1]["deliveries"]} доставок**; все научные экспорты и полный payload NSCP совпали. Сценарий с загрузкой, выгрузкой и сравнением занял **{long["wall_seconds"]:.2f} с**. Здесь выигрыш меньше, чем на начальном участке; переносить ранние проценты на весь опыт нельзя.', '',
 'Отдельные расходы новой обычной сборки на 8 потоках, медианы 3 повторов (секунды):','',
 '| Окно | Загрузка и создание Simulator | Экспорт результатов | Сохранение NSCP | Анализ RAM без экспорта |','|---|---:|---:|---:|---:|']
for window in ['quiet','burst']:
 rows=[r for r in new if r['window']==window and r['requested_threads']==8]
 more.append(f'| {window} | {med(rows,"setup_seconds"):.4f} | {med(rows,"export_seconds"):.4f} | {med(rows,"checkpoint_seconds"):.4f} | {med(rows,"analysis_seconds"):.6f} |')
more+=['','Общий экспорт включает стандартный аналитический экспорт; отдельно измеренный анализ выше не включает запись его файлов. Для quiet загружается JSON, для burst — совместимый NSCP. Эти времена не включены в solve_seconds.','']
lines[i:i]=more

p.joinpath('REPORT_RU.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
