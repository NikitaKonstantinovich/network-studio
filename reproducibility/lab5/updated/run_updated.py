"""Prepared Lab5 protocols for the new triplet-capable native CLI.

Default invocation only prepares exact configs. No model runs without --run.
Use --cli <verified new exe>; old pair baseline remains separately frozen.
"""
from __future__ import annotations
import argparse
import bisect
import copy
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import time

import numpy as np

HERE=Path(__file__).resolve().parent
PAIRS=HERE.parent/'pairs'
STUDIO=HERE.parents[2]
spec=importlib.util.spec_from_file_location('lab5_pairs_baseline',PAIRS/'run.py')
baseline=importlib.util.module_from_spec(spec);spec.loader.exec_module(baseline)
sha,load,save=baseline.sha,baseline.load,baseline.save

EXTERNAL=dict(baseline.EXTERNAL)
EXTERNAL.update({'t2a_triplet_pre_post_1Hz':.119444,'t2a_triplet_pre_post_10Hz':.459589,
                 't2b_pair_pre_post_pre_1Hz':.1141,'t2b_triplet_pre_post_pre_1Hz':.0887917,
                 't2b_pair_post_pre_post_1Hz':.141625,'t2b_triplet_post_pre_post_1Hz':.1523})

def canonical(c):
    """Only normalize schema/default optional fields; keep all model parameters."""
    c=copy.deepcopy(c);c.pop('schema',None)
    for n in c['neurons']:n.setdefault('pulse_intervals',[])
    for s in c['synapses']:s.setdefault('tau_slow',.1)
    return c

def cases():
    protocol=[]
    for name,dt,c in baseline.configurations():
        if dt!=25e-6:continue
        c=copy.deepcopy(c)
        for n in c['neurons']:n['pulse_intervals']=[]
        c['synapses'][0]['tau_slow']=.1
        protocol.append((name,c,name))
        if name.startswith('t2a'):
            triplet=copy.deepcopy(c);triplet['synapses'][0]['stdp_rule']='triplet'
            triplet_name=name.replace('t2a_pre_post','t2a_triplet_pre_post')
            protocol.append((triplet_name,triplet,None))
    template=copy.deepcopy(next(c for name,c,old in protocol if name=='t2a_pre_post_1Hz'))
    for sequence in ('pre_post_pre','post_pre_post'):
        for rule in ('pair','triplet'):
            c=copy.deepcopy(template)
            for n in c['neurons']:n.update(start=0.,frequency=1)
            if sequence=='pre_post_pre':offsets=([.05,.07],[.06])
            else:offsets=([.06],[.05,.07])
            for n,starts in zip(c['neurons'],offsets):
                n['pulse_intervals']=[{'start':t,'stop':t+.003} for t in starts]
            c['synapses'][0]['stdp_rule']='triplet' if rule=='triplet' else 'multiplicative'
            protocol.append((f't2b_{rule}_{sequence}_1Hz',c,None))
    result=[]
    for name,base,old in protocol:
        for dt in (25e-6,12.5e-6):
            c=copy.deepcopy(base);c['name']='Lab5 '+name;c['dt']=dt
            result.append({'name':name,'config':c,'workload':'official','baseline_case':old})
    for name,base,old in protocol:
        if name.startswith('t1'):continue
        c=copy.deepcopy(base);c['name']='Lab5 width1 control '+name;c['dt']=25e-6
        for n in c['neurons']:
            n['width']=.001
            for interval in n['pulse_intervals']:interval['stop']=interval['start']+.001
        result.append({'name':'width1_'+name,'config':c,'workload':'controls','baseline_case':None})
    assert len(protocol)==14 and len(result)==36
    return result

def label_for(case):return (case['name']+f"_dt{case['config']['dt']*1e6:g}us").replace('.','p')

def direct_replay(events,s,sample_times):
    assert len(set(events))==len(events)
    pre=np.array([t for t,n in events if n==1]);post=np.array([t for t,n in events if n==2])
    groups={}
    for t,n in events:groups.setdefault(t,[False,False])[n-1]=True
    w=s['weight'];timeline=[(0.,w)];maximum_pairs=0;ltp_total=ltd_total=0.;same_time=0
    for t,(is_pre,is_post) in sorted(groups.items()):
        pre_end=int(np.searchsorted(pre,t,side='left'));post_end=int(np.searchsorted(post,t,side='left'))
        prior_pre=float(np.exp(-(t-pre[:pre_end])/s['tau_plus']).sum()) if is_post else 0.
        prior_post=float(np.exp(-(t-post[:post_end])/s['tau_minus']).sum()) if is_pre else 0.
        if is_post and s['stdp_rule']=='triplet':
            prior_post_slow=float(np.exp(-(t-post[:post_end])/s['tau_slow']).sum())
        else:prior_post_slow=1.
        # Both factors use the same pre-update weight. The current post event
        # does not participate in its own slow postsynaptic history.
        ltp=s['a_plus']*(s['weight_max']-w)*prior_pre*prior_post_slow
        ltd=s['a_minus']*(w-s['weight_min'])*prior_post
        w=min(s['weight_max'],max(s['weight_min'],w+ltp-ltd))
        ltp_total+=ltp;ltd_total+=ltd;same_time+=int(is_pre and is_post)
        timeline.append((t,w));maximum_pairs+=pre_end*int(is_post)+post_end*int(is_pre)
    tt=[t for t,w in timeline];ww=[w for t,w in timeline]
    values=[ww[bisect.bisect_right(tt,t+1e-12)-1] for t in sample_times]
    return values,{'event_groups':len(groups),'strictly_earlier_fast_pairs_enumerated':maximum_pairs,
                   'same_time_groups':same_time,'summed_LTP':ltp_total,'summed_LTD':ltd_total,
                   'rule':s['stdp_rule'],'slow_trace_increment_after_update':s['stdp_rule']=='triplet'}

def oracle_self_test():
    s={'weight':.1,'weight_min':0.,'weight_max':1.,'a_plus':.001,'a_minus':.001,
       'tau_plus':.01,'tau_minus':.01,'tau_slow':.1,'stdp_rule':'triplet'}
    e=np.exp
    after_ltd=.1-.001*.1*float(e(-1))
    fixtures=[
        ('isolated_pair_no_triplet_LTP',[(0.,1),(.01,2)],.1),
        ('same_time_no_self_pair',[(0.,1),(0.,2)],.1),
        ('pre_post_pre',[(0.,1),(.01,2),(.02,1)],after_ltd),
        ('post_pre_post',[(0.,2),(.01,1),(.02,2)],after_ltd+.001*(1-after_ltd)*float(e(-1.2))),
        ('same_time_with_prior_history',[(0.,2),(.01,1),(.02,1),(.02,2)],
             after_ltd+.001*(1-after_ltd)*float(e(-1.2))-.001*after_ltd*float(e(-2))),
        ('all_previous_presynaptic_spikes',[(0.,1),(.005,1),(.01,2),(.015,2)],
             .1+.001*.9*float((e(-1.5)+e(-1))*e(-.05))),
    ]
    output=[]
    for name,events,expected in fixtures:
        actual=direct_replay(events,s,[.1])[0][0]
        assert abs(actual-expected)<1e-14,(name,actual,expected)
        output.append({'case':name,'expected':expected,'actual':actual,'absolute_error':abs(actual-expected)})
    s['stdp_rule']='multiplicative'
    actual=direct_replay([(0.,1),(.01,2)],s,[.1])[0][0]
    expected=.1+.001*.9*float(e(-1));assert abs(actual-expected)<1e-14
    output.append({'case':'multiplicative_pair_control','expected':expected,'actual':actual,'absolute_error':abs(actual-expected)})
    save(HERE/'oracle_self_test.json',{'scope':'Analytical tests of the independent Python replay, not tests of the C++ implementation.','passed':True,'cases':output})

def execute(case,cli,skip=False):
    name=label_for(case);config=HERE/'configs'/(name+'.json');prefix=HERE/'raw'/name
    c=case['config'];runfile=HERE/'raw'/(name+'_run.json')
    if skip or runfile.exists():
        result=load(runfile)
        assert result['config_sha256']==sha(config)
        if cli is not None:assert result['cli_sha256']==sha(cli),'Binary changed; use a separate result directory.'
    else:
        assert cli is not None
        cli_hash=sha(cli);started=time.perf_counter()
        proc=subprocess.run([str(cli),'--config',str(config),'--output',str(prefix)],capture_output=True,text=True)
        seconds=time.perf_counter()-started
        Path(str(prefix)+'.log').write_text(proc.stdout+proc.stderr,encoding='utf-8')
        if proc.returncode:raise RuntimeError(name+': '+proc.stderr)
        assert sha(cli)==cli_hash,'CLI was replaced during a run'
        result={'cli_path':str(cli),'cli_sha256':cli_hash,'config_sha256':sha(config),'wall_seconds':seconds}
        save(runfile,result)
        print(json.dumps({'completed':name,'wall_seconds':round(seconds,3)}),flush=True)
    snapshot=load(str(prefix)+'_experiment.json')
    assert canonical(snapshot)==canonical(c),'Configuration snapshot changed: '+name
    summary=load(str(prefix)+'_summary.json')
    assert summary['complete'] and not summary['error'] and summary['time_s']==100
    hashes={suffix:sha(str(prefix)+'_'+suffix) for suffix in ('experiment.json','summary.json','trace.csv','events.csv','weights.csv')}
    if 'raw_sha256' in result:assert hashes==result['raw_sha256'],'Raw output changed: '+name
    result['raw_sha256']=hashes;save(runfile,result)
    return prefix,result,summary

def analyze(case,prefix,run,summary):
    c=case['config'];name=case['name'];n=c['neurons'];s=c['synapses'][0]
    with Path(str(prefix)+'_events.csv').open(encoding='utf-8') as stream:rows=list(csv.DictReader(stream))
    assert all(r['type']=='spike' for r in rows)
    events=[(float(r['time_s']),int(r['node_index_1based'])) for r in rows]
    assert events==sorted(events)
    counts=[sum(node==i for t,node in events) for i in (1,2)]
    assert counts==summary['neuron_spikes']
    weights=np.genfromtxt(str(prefix)+'_weights.csv',delimiter=',',names=True)
    expected,oracle=direct_replay(events,s,weights['time_s'])
    error=float(np.max(abs(weights['S1_weight']-expected)))
    assert error<1e-9,(name,'Independent all-history STDP discrepancy',error)
    oracle.update(max_sample_weight_abs_error=error,final_weight=expected[-1])
    final=float(weights['S1_weight'][-1]);assert abs(final-summary['final_synapse_weights'][0])<1e-15
    checkpoints={}
    for t in (98,99,100):
        idx=np.flatnonzero(abs(weights['time_s']-t)<1e-9);assert len(idx)==1
        checkpoints[str(t)]={'weight':float(weights['S1_weight'][idx[0]]),
            'spikes':[sum(node==i and et<=t+1e-12 for et,node in events) for i in (1,2)]}
    baseline_match=None
    if case['baseline_case']:
        oldlabel=(case['baseline_case']+f"_dt{c['dt']*1e6:g}us").replace('.','p')
        oldprefix=PAIRS/'raw'/oldlabel
        baseline_match={suffix:sha(str(oldprefix)+'_'+suffix)==sha(str(prefix)+'_'+suffix)
                        for suffix in ('trace.csv','events.csv','weights.csv')}
        baseline_match['configuration_same_after_optional_defaults']=canonical(load(str(oldprefix)+'_experiment.json'))==canonical(load(str(prefix)+'_experiment.json'))
        assert all(baseline_match.values()),(name,'Pair baseline regression',baseline_match)
    result={'case':name,'workload':case['workload'],'label':label_for(case),'dt_us':c['dt']*1e6,'width_ms':n[0]['width']*1000,
        'rule':s['stdp_rule'],'gain':s['gain'],'final_weight':final,'spike_counts':counts,'checkpoints':checkpoints,
        'cycles':[baseline.cycles([t for t,node in events if node==i],n[0]['frequency']) for i in (1,2)],
        'stdp_replay':oracle,'baseline_byte_matches':baseline_match,'provenance':run,'snapshot_exact_after_default_normalization':True}
    if case['workload']=='official':
        external=EXTERNAL[name];result.update(external_report_weight=external,difference_from_external=final-external)
    else:
        result['note']='Changed1ms diagnostic stimulus; not reproduction of the manual3ms protocol.'
        expected_per_frame=[len(node['pulse_intervals']) if node['pulse_intervals'] else 1 for node in n]
        result['expected_stimuli_per_frame']=expected_per_frame
        result['all_frames_have_expected_spike_count']=[set(row['counts_per_frame'])=={str(expected_count)}
              for row,expected_count in zip(result['cycles'],expected_per_frame)]
    save(HERE/'raw'/(label_for(case)+'_analysis.json'),result)
    return result

def write_report(summary):
    rows=summary['rows'];official=[r for r in rows if r['workload']=='official']
    controls=[r for r in rows if r['workload']=='controls']
    hashes=sorted({r['provenance']['cli_sha256'] for r in rows})
    replay_error=max(r['stdp_replay']['max_sample_weight_abs_error'] for r in rows)
    matches=[r for r in rows if r['baseline_byte_matches'] is not None]
    matched_csv=sum(sum(bool(r['baseline_byte_matches'][suffix]) for suffix in ('trace.csv','events.csv','weights.csv')) for r in matches)
    text=['# ЛР5: парное и минимальное триплетное правила',
      '',f"В этой серии завершено {len(rows)} запусков по 100 с. SHA256 использованной CLI: `"+'`, `'.join(hashes)+'`.',
      '', 'Основные протоколы сохраняют исходные импульсы25×3мс. Диагностические контроли25×1мс явно отделены и не являются повторением методички. Начальные V=−65мВ,u=−13, независимый сброс, Noise0, TM и параметры обучения — в точных JSON `configs/`.',
      '', 'Минимальное триплетное правило: при постспайке LTP=A+·(wmax−w)·pre_fast·post_slow_old; при пресинаптическом событии LTD=A−·(w−wmin)·post_fast. Tau_slow=.1с; текущий постспайк добавляется после обновления. Отдельного парного слагаемого A2+ нет. Это определённая математическая модель; равенство закрытой реализации NeuroNet отдельно не установлено.',
      '', '## Основные опыты: импульсы3мс', '',
      '| Протокол | NeuroNet | Вес25мкс | Вес12.5мкс | Спайки пре/пост25мкс |',
      '|---|---:|---:|---:|---:|']
    for name in dict.fromkeys(r['case'] for r in official):
        pair=[r for r in official if r['case']==name]
        coarse=next((r for r in pair if r['dt_us']==25),None)
        fine=next((r for r in pair if r['dt_us']==12.5),None)
        if coarse and fine:
            text.append(f"|{name}|{coarse['external_report_weight']:.9g}|{coarse['final_weight']:.9g}|{fine['final_weight']:.9g}|{coarse['spike_counts'][0]}/{coarse['spike_counts'][1]}|")
    text+=['', 'Веса98/99/100с, гистограммы реального числа спайков в каждом полном кадре и полные журналы событий сохранены в findings и raw. Подсчёт по кадрам не доказывает причинную связь отдельного спайка с импульсом. Оригинальные опыты имели Step0.500мс, приблизительную длительность98–101с и недокументированные здесь точные начальные состояния/задержки. Две наши сетки являются проверкой чувствительности, а не доказательством достижения непрерывного предела.',
      '', '## Диагностические опыты: импульсы1мс', '',
      '| Протокол | Вес25мкс | Спайки пре/пост | Ожидаемое число на каждый кадр достигнуто, пре/пост |',
      '|---|---:|---:|---|']
    for r in controls:
        text.append(f"|{r['case']}|{r['final_weight']:.9g}|{r['spike_counts'][0]}/{r['spike_counts'][1]}|{r['all_frames_have_expected_spike_count']}|")
    text+=['', 'Флаг в последнем столбце проверяется по фактическим событиям. False означает, что даже изменённый стимул не создал идеальную запланированную последовательность; результаты такого случая нельзя объяснять только различием правил обучения.',
      '', '## Независимая проверка и регрессия', '',
      f'Максимальное абсолютное расхождение независимого replay с записанными весами — {replay_error:.3g}. Replay непосредственно суммирует все строго прошлые события, без повторения рекуррентного алгоритма C++-следов. Семь аналитических проверок Python-oracle сохранены отдельно; сами по себе они не являются тестами C++.',
      '', f'Для {len(matches)} исходных парных запусков совпали побайтно {matched_csv} CSV состояний/событий/весов с замороженной прежней сборкой. Snapshot сравнивается после добавления только пустых pulse_intervals и tau_slow=.1 по умолчанию; все остальные параметры должны совпадать.',
      '', 'Для внешнего pre→post→pre триплетного значения0.0887917 исходный отчёт отдельно оговаривает неопределённость непрерывного включения режима. Это значение не следует использовать как безусловно подтверждённую цель настройки.',
      '', '## Повторение', '', 'Из корня cudaynamics, с NumPy в PYTHONPATH:', '', '```powershell',
      "$env:PYTHONPATH='E:/asp/26_27/tmp/python_deps'",
      "& 'C:/Users/N1kit/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -B standalone/network_studio/reproducibility/lab5/updated/run_updated.py --run --workload all --cli standalone/network_studio/build/Release/network_studio_cli.exe",
      '# Только анализ сохранённого результата: заменить --run на --skip-run.', '```', '',
      'Скрипт проверяет неизменность SHA, точные конфигурации, завершение100с, счётчики событий и независимый весовой replay. Уже завершённые запуски используются повторно только при совпадении SHA входов, бинарного файла и CSV. Переиспользование другой сборки поверх имеющихся результатов запрещено. Без --run или --skip-run выполняется только подготовка конфигураций.']
    (HERE/'README_RU.md').write_text('\n'.join(text)+'\n',encoding='utf-8')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--run',action='store_true');ap.add_argument('--skip-run',action='store_true')
    ap.add_argument('--cli',type=Path);ap.add_argument('--workload',choices=('official','controls','all'),default='official')
    args=ap.parse_args();assert not(args.run and args.skip_run)
    oracle_self_test()
    selected=[c for c in cases() if args.workload=='all' or c['workload']==args.workload]
    (HERE/'configs').mkdir(exist_ok=True);(HERE/'raw').mkdir(exist_ok=True)
    for case in cases():
        path=HERE/'configs'/(label_for(case)+'.json')
        if path.exists():assert load(path)==case['config'],'Prepared protocol changed: '+str(path)
        else:save(path,case['config'])
    save(HERE/'planned_protocols.json',{'official_protocols':14,'official_runs':28,'width1_control_runs':8,
          'duration_s':100,'cases':[{'name':c['name'],'label':label_for(c),'workload':c['workload'],
             'config_sha256':sha(HERE/'configs'/(label_for(c)+'.json'))} for c in cases()]})
    if not(args.run or args.skip_run):
        print('Prepared36 configurations; no CLI execution. Await verified triplet build.');return
    cli=args.cli.resolve() if args.cli else None
    if args.run:
        assert cli is not None and cli.is_file(),'--run requires --cli <verified new executable>'
        assert sha(cli)!=baseline.EXPECTED_SHA,'Old baseline executable has no triplet support'
    output=[]
    for case in selected:output.append(analyze(case,*execute(case,cli,args.skip_run)))
    summary={'scope':'14 original25x3ms protocols at2steps;8 separate25x1ms diagnostic controls. Minimal multiplicative triplet rule, no all-network or NeuroNet equivalence claim.',
         'workload':args.workload,'runs':len(output),'rows':output,'user_report':str(baseline.USER_REPORT),
         'user_report_sha256':sha(baseline.USER_REPORT),'planned_protocols_sha256':sha(HERE/'planned_protocols.json'),
         'limitations':['All-time-pair replay verifies learning on supplied native events; it does not validate underlying event accuracy.',
             'The original NeuroNet triplet implementation, conduction delay and exact initial conditions are not established.',
             'External pre-post-pre triplet weight.0887917 has uncertain uninterrupted rule selection per user report.',
             'Width1ms controls explicitly alter the experiment and may still fail to produce the requested number of spikes; actual counts are reported.']}
    save(HERE/f'findings_{args.workload}.json',summary)
    write_report(summary)
    print(json.dumps({'runs':len(output),'max_replay_error':max(r['stdp_replay']['max_sample_weight_abs_error'] for r in output)},indent=2))

if __name__=='__main__':main()
