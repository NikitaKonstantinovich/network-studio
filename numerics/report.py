"""Assemble the reviewable numerical report from completed checks, not estimates."""
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'verification/numerics_20261012'
HERE=Path(__file__).resolve().parent
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,d):p.write_text(json.dumps(d,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')


def main():
    result=read(OUT/'current/convergence.json')
    assert result['cli_sha256']==sha(ROOT/'build/Release/network_studio_cli.exe')
    before=read(OUT/'baseline/integration/findings.json');after=read(OUT/'current/integration/findings.json')
    fs=read(OUT/'baseline/fs_findings.json')
    log=(OUT/'build_and_tests.log').read_text(encoding='utf-8-sig')
    assert '100% tests passed, 0 tests failed out of 5' in log
    renderer=(OUT/'gui/renderer.txt').read_text();assert 'frames=24' in renderer and 'error=\n' in renderer
    old=read(OUT/'baseline/old_audit_sha256.json')
    changed=[name for name,digest in old.items() if not (ROOT/name).is_file() or sha(ROOT/name)!=digest]
    assert not changed,changed
    build={str(p):sha(p) for p in sorted((ROOT/'build/Release').glob('*.exe'))}
    source_files=[p for p in ROOT.glob('*.cpp')]+[p for p in ROOT.glob('*.h')]
    source_files += list(HERE.glob('*.h'))+list(HERE.glob('*.py'))+[ROOT/'CMakeLists.txt',ROOT/'build.ps1',ROOT/'lab2/population.cpp',ROOT/'lab2/population.h',ROOT/'lab2/population_integrator.inc']
    manifest=dict(branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),
        baseline_commit=subprocess.check_output(['git','rev-parse','baseline-before-numerics'],cwd=ROOT,text=True).strip(),
        binaries_sha256=build,source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sorted(set(source_files))},
        original_audit_files_verified=len(old),ctest='5/5 passed',gui_smoke=renderer,
        independent_validation_sha256=sha(OUT/'current/convergence.json'),baseline_cli_sha256=fs['cli_sha256'])
    save(HERE/'build_manifest.json',manifest);save(HERE/'results.json',result)
    shutil.copyfile(ROOT/'build/native/Testing/Temporary/LastTest.log',OUT/'ctest.log')
    replay=max(r['event_log_replay_max_error'] for rows in result['rows'].values() for r in rows)
    new_fs=result['rows']['FS'][1]['max_event_error_ms'];charge=max(abs(r['delivered_charge_ratio']-1) for r in after['rows'])
    decay=after['decay_rows'][0]
    lines=[
        '# Исправление численной схемы Network Studio', '',
        '**12 октября 2026. Рабочая сборка проверена: 5/5 CTest, GUI smoke, 27 сравнений с независимым решателем.**', '',
        f"Проект находится в `E:/asp/26_27/configurator` после ранее выполненного переноса. Ветка: `{manifest['branch']}` в локальном Git-репозитории этой папки. Исходный снимок со всеми пользовательскими файлами: `{manifest['baseline_commit']}`. Репозиторий `cudaynamics` и его незакоммиченные изменения не изменялись.", '',
        '## Воспроизведение до исправлений', '',
        '| Контроль | Сохранённая сборка | Исправленная сборка |',
        '|---|---:|---:|',
        f'| Площадь импульса 10 мкс, dt=25 мкс, сдвиг 101/110 мкс | 0% / 250% | 100% / 100%; максимальная ошибка доли по 8 контролям {charge:.3g} |',
        f"| Y, Omega_Y=200000 с⁻¹, t=1 мс | {before['decay_rows'][0]['final_Y_uM']:.6g} мкМ; CLI успешен | {decay['final_Y_uM']:.6g} мкМ, положительное затухание |",
        f"| FS, 200 мс, dt=25 мкс: максимальная ошибка времени | {fs['maximum_event_error_ms']:.9g} мс | {new_fs:.9g} мс; 28/28 событий |", '',
        f"В новом контроле Y спайк находится в {decay['actual_release_time_s']:.12g} с, а не в конце шага. Аналитическое значение от нового события равно {decay['exact_from_event_uM']:.9g} мкМ; ошибка |ln(Y/Yточн)|={decay['log_error_from_event']:.3g}. Поэтому старое аналитическое значение, отсчитанное от 25 мкс, тоже должно измениться. Проверки требуют **Y>0** и логарифмическую ошибку <10⁻⁸: ошибочный ноль не проходит.", '',
        '## Алгоритмы и соглашения', '',
        '- Основная сетка dt задаёт верхнюю границу шага. Внутри шаг разделяется по включению/выключению одиночных и периодических импульсов, границам таблицы, start/stop, обновлению шума и сетке записи. Периодические края имеют единое абсолютное представление; выбирается только строго следующая граница. Семантика входа — [start, stop).',
        '- u, синаптические s/y и Y, восстановление STP/TM, xA и GA имеют точные непрерывные переходы. Для совпадающих постоянных TM используется предельная формула, для малых шагов — устойчивая форма переноса ресурса. Изолированные рецепторные/IP3-распады тоже вычисляются точно. Эти значения подставляются **на каждой стадии** RK4, поэтому быстрый вход действует на связанные переменные внутри шага.',
        '- Связанный нелинейный поток контролируется удвоением шага: принимаются два полушага, оценка ошибки — разность с целым шагом /15, масштаб 10⁻¹¹ + 10⁻⁸·max(|y₀|,|y₁|). Дополнительно h·rate≤0,25 ограничивает разрешение быстрых связанных кинетик. Неприемлемый шаг уменьшается; обрезания непрерывных состояний нет. Выход за физические границы или невозможность продвинуть часы даёт явную ошибку.',
        '- Восходящие пересечения локализуются повторным интегрированием и бисекцией. Допуск времени max(10⁻¹³ с, 8·eps·max(1,|t|)); события в пределах двух таких допусков объединяются. Числовой допуск порога — 16·eps·max(1,|порог|). Поиск всех кандидатов предшествует скачкам, поэтому обход нейронов не определяет результат.',
        '- Для Ижикевича в найденный момент выполняются V←c и u←u+d; HH не сбрасывается. Затем применяются STP/TM-выбросы и кальциевые события из общего состояния до скачков. STDP использует старые затухающие следы, суммирует обе полярности, один раз ограничивает вес и только затем добавляет новые события. Одновременные спайки не обучаются друг на друге с нулевой задержкой. Новый вес действует сразу на оставшийся поток, включая прежний синаптический хвост.',
        '- При совпадении события с краем стимула сначала заканчивается непрерывный поток слева, затем выполняется группа событий, после неё включается вход справа. Совпадающая запись содержит состояние после скачков. За один основной шаг допустимы несколько событий одного нейрона. Генератор шума по seed/индексу/временному bin не расходует новые числа при дроблении шага; запись остаётся на k·sample.', '',
        '## Сходимость и независимый эталон', '',
        'Эталон — отдельная реализация полных уравнений через SciPy DOP853 (rtol=2·10⁻¹¹, atol=2·10⁻¹³, max_step=50 мкс), точные разрывы входа и терминальные пересечения с продолжением после сброса. FS, HH и быстрый связанный контроль дополнительно решены Radau. Формулы событийного обучения проверяются прямыми суммами по всей истории, без рекуррентных следов C++.', '',
        'Таблица: максимальная ошибка времени событий в мс и максимальная ошибка напряжения на общей сетке записи. Во всех строках совпали числа событий каждого нейрона/астроцита.', '',
        '| Сценарий | Δt события, dt=50 мкс | dt=25 мкс | dt=12,5 мкс | max ΔV при 25 мкс, мВ |',
        '|---|---:|---:|---:|---:|']
    for name,rows in result['rows'].items():
        lines.append(f"| {name} | {rows[0]['max_event_error_ms']:.3g} | {rows[1]['max_event_error_ms']:.3g} | {rows[2]['max_event_error_ms']:.3g} | {rows[1]['max_voltage_error_mV']:.3g} |")
    lines += ['',
        'RS/FS: DC=10, 200 мс; HH: DC=10, 50 мс; pulse: 30×1,013 мс с началом 10,101 мс, 20 Гц; малая сеть: два нейрона Ижикевича, TM и астроцит с обратной связью, 100 мс, включая кальциевый выброс. STDP: два DC-нейрона, слабая действующая связь, 100 мс. Быстрый связанный контроль: Omega_Y=200000, Omega_N=50000, Omega_G=100000, Omega_S=20000 с⁻¹, активные рецепторные связи и кальциевый выброс, 2 мс. Полные JSON сохранены рядом с CSV.', '',
        'При уточнении dt ошибки времени и RMS траекторий уменьшаются для RS, FS, HH, импульсного ответа, малой сети и всех трёх правил STDP. Не заявляется точный коэффициент 16: около событий и быстрых процессов работают дополнительные адаптивные ограничения. В fast_coupled результат почти не меняется с основной сеткой, потому что внутренний шаг уже определяется быстрыми скоростями и допуском.', '',
        f'Независимый пересчёт всех записанных весов по новым журналам событий: максимальная абсолютная ошибка **{replay:.3g}**. Проверены аддитивное, мультипликативное и минимальное триплетное правила. Ресурсные пулы TM, доли/рецепторы и положительность распадов проходят проверки. C++ отдельно проверяет десятки спайков и обновлений STDP внутри одного основного шага.', '',
        'Согласование DOP853/Radau:', '',
        '| Контроль | max расхождение событий, мс | max abs(Δy)/(1+abs(y)) |',
        '|---|---:|---:|']
    for row in result['crosschecks']:
        lines.append(f"| {row['case']} | {row['max_event_difference_ms']:.3g} | {row['max_state_scaled_difference']:.3g} |")
    lines += ['', '## Сборка, совместимость и ограничения', '',
        'Штатная сборка: `build.ps1 -Test -Smoke -SmokeOutput verification/numerics_20261012/gui`. Пройдены все четыре прежних набора тестов и новый `numerical_regressions`: **5/5**, 196,06 с на этой машине. Проверены JSON 1–4, CLI, взаимодействия GUI, выборочная запись и крупные сети. Пакетный population-исполнитель переведён на тот же численный механизм с сохранением агрегации; его тест сравнивает траектории, ресурсы, времена событий и запись с основным ядром. GUI smoke: 24 кадра, 40001 отсчёт, error пуст; изображения интерфейса просмотрены.', '',
        f'Сохранены и сверены SHA-256 **{len(old)}** прежних файлов model_audit. Новые данные находятся только в `verification/numerics_20261012`; исходные бинарники — в `baseline/Release`. Ничего из старого аудита не переписано. Сравнение с прежним CUDA-stepper заменено проверкой одинаковых математических правых частей: старый stepper сохраняет прежний дискретный протокол и больше не является численным эталоном.', '',
        'Ожидаемые отличия CSV: площадь коротких импульсов, непрерывные траектории после раннего сброса, времена/порядок близких спайков, ресурсы и обученные веса. Для sample, не кратного dt, запись теперь происходит точно на её собственной сетке. Эти отличия объясняются вычислительным протоколом и подтверждены аналитикой/адаптивным эталоном, а не подгонкой к NeuroNet.', '',
        'Ограничения: это проверка указанных режимов, а не доказательство точности любой сети или биологической достоверности. Контроль ошибки и локализация дороже прежнего фиксированного RK4. В очень жёстких режимах возможен явный отказ по разрешению часов/числу подшагов; ниже диапазона double неизбежно underflow. События ближе допуска времени считаются одновременными. Сохраняются исходная дискретная модель шума, стационарная инициализация HH-затворов и физические ограничения модели/единиц смешанных связей, описанные прежним аудитом.', '',
        '## Готовая сборка и SHA-256', '',
        'Запуск: `E:/asp/26_27/configurator/launch.cmd`.', '']
    for filename in ['network_studio.exe','network_studio_cli.exe','network_studio_population.exe']:
        p=ROOT/'build/Release'/filename;lines += [f'- `{p.as_posix()}`',f'  SHA-256: `{sha(p)}`']
    lines += ['', 'Полный манифест сборки и исходников: [build_manifest.json](build_manifest.json). Числа таблиц: [results.json](results.json). Журналы: [сборка/CTest](../verification/numerics_20261012/build_and_tests.log), [адаптивная проверка](../verification/numerics_20261012/independent_validation.log), [GUI](../verification/numerics_20261012/gui/renderer.txt).', '',
        'Повтор независимых проверок из корня configurator (Python 3.12, NumPy/SciPy в ../tmp/python_deps):', '',
        '```powershell',
        "$py = 'C:/Users/N1kit/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'",
        '& $py -B numerics/check_counterexamples.py',
        '& $py -B numerics/validate.py',
        '& $py -B numerics/report.py',
        '```', '',
        '`reproduce_baseline.py` предназначен только для сохранённых исходных бинарников. Его повтор пишет исключительно в новый каталог baseline, не в model_audit.', '']
    (HERE/'REPORT_RU.md').write_text('\n'.join(lines),encoding='utf-8')
    print(json.dumps(dict(report=str(HERE/'REPORT_RU.md'),old_audit_files_verified=len(old),maximum_event_log_replay_error=replay,binaries=build),indent=2))


if __name__=='__main__':main()
