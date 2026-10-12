"""Lab5 task1 and task2A pair-STDP experiments on the unchanged native CLI.

All outputs remain beside this script. Independent replay enumerates strictly
earlier spike pairs, never reuses the core's recursive trace algorithm.
"""
from __future__ import annotations
import argparse
import bisect
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

import numpy as np

HERE=Path(__file__).resolve().parent
STUDIO=HERE.parents[2]
CLI=HERE/'baseline/network_studio_cli.exe'
EXPECTED_SHA='4965bb5cc41f13f243bf9823f9c3d6c68c1ab0a77d1677574007003f50342a8e'
USER_REPORT=STUDIO.parent/'labs/lab4_report/Отчёт_ЛР4.md'
EXTERNAL={
    't1_pre_post_1Hz':.501333,'t1_post_pre_1Hz':.0157814,
    't1_pre_post_10Hz':.999748,'t1_post_pre_10Hz':.0000803577,
    't1_pre_post_40Hz':.335617,'t1_post_pre_40Hz':.153155,
    't2a_pre_post_1Hz':.147829,'t2a_pre_post_10Hz':.5437,
}

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def load(path): return json.loads(Path(path).read_text(encoding='utf-8'))
def save(path,obj): Path(path).write_text(json.dumps(obj,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')

def configurations():
    base=load(HERE/'baseline/stdp_manual.json')
    base.update(schema=4,record_all=True,record_neurons=[],record_synapses=[],duration=100.,sample=.1,noise_sigma=0.)
    for n in base['neurons']:
        n.update(voltage=-65,current=0,amplitude=25,stop=100.,width=.003,
                 izh_a=.02,izh_b=.2,izh_c=-65,izh_d=8,izh_peak=30,izh_u=-13,
                 equilibrium_u=True,individual_noise=False,noise_sigma=0.,inhibitory=False)
    s=base['synapses'][0]
    s.update(model='tsodyks_markram',source=0,target=1,astro=-1,release=.5,tau_d=.05,tau_syn=.01,
             tau_f=1.,weight=.1,gain=20.,stdp=True,stdp_rule='multiplicative',
             a_plus=.001,a_minus=.005,tau_plus=.01,tau_minus=.01,weight_min=0.,weight_max=1.)
    cases=[]
    for task,frequencies in [('t1',(1,10,40)),('t2a',(1,10))]:
        for f in frequencies:
            for order in ('pre_post','post_pre') if task=='t1' else ('pre_post',):
                name=f'{task}_{order}_{f}Hz'
                for dt in (25e-6,12.5e-6):
                    c=copy.deepcopy(base);c['dt']=dt;c['name']='Lab5 '+name
                    starts=(.005,.015) if f==40 else (.05,.06)
                    if order=='post_pre': starts=starts[::-1]
                    for node,start in zip(c['neurons'],starts):node.update(frequency=f,start=start)
                    if task=='t2a':c['synapses'][0].update(gain=2.,a_minus=.001)
                    cases.append((name,dt,c))
    return cases

def run_case(name,dt,c,skip=False):
    label=f'{name}_dt{dt*1e6:g}us'.replace('.','p')
    config=HERE/'configs'/(label+'.json');prefix=HERE/'raw'/label
    (HERE/'configs').mkdir(exist_ok=True);(HERE/'raw').mkdir(exist_ok=True)
    if config.exists(): assert load(config)==c,'Existing configuration changed: '+label
    else:save(config,c)
    provenance=HERE/'raw'/(label+'_run.json')
    if skip:
        assert provenance.exists(),'Missing completed run '+label
        old=load(provenance)
        assert old['cli_sha256']==EXPECTED_SHA
        assert old['config_sha256']==sha(config),'Configuration bytes changed: '+label
    else:
        started=time.perf_counter()
        process=subprocess.run([str(CLI),'--config',str(config),'--output',str(prefix)],capture_output=True,text=True)
        seconds=time.perf_counter()-started
        Path(str(prefix)+'.log').write_text(process.stdout+process.stderr,encoding='utf-8')
        if process.returncode:raise RuntimeError(label+': '+process.stderr)
        old={'label':label,'wall_seconds':seconds,'cli_sha256':sha(CLI),'config_sha256':sha(config)}
        save(provenance,old)
        print(json.dumps({'completed':label,'wall_seconds':round(seconds,3)},ensure_ascii=False),flush=True)
    snapshot=load(str(prefix)+'_experiment.json')
    assert snapshot==c,'Native configuration mismatch: '+label
    summary=load(str(prefix)+'_summary.json')
    assert summary['complete'] and not summary['error'] and summary['time_s']==100
    rawfiles={suffix:sha(str(prefix)+'_'+suffix) for suffix in ('experiment.json','summary.json','trace.csv','events.csv','weights.csv')}
    if skip and 'raw_sha256' in old:assert old['raw_sha256']==rawfiles,'Historical raw changed: '+label
    old['raw_sha256']=rawfiles
    save(provenance,old)
    return label,prefix,old,summary

def replay(events,synapse,sample_times):
    # Array-backed direct sums over EVERY strictly previous event. This avoids
    # both recursive pre/post traces and the incremental core decay algorithm.
    assert len(set(events))==len(events),'Duplicate same-neuron events at one timestamp'
    pre=np.array([t for t,n in events if n==1]);post=np.array([t for t,n in events if n==2])
    groups={}
    for t,n in events:groups.setdefault(t,[False,False])[n-1]=True
    w=synapse['weight'];timeline=[(0.,w)];updates=[];maximum_pairs=0
    for t,(is_pre,is_post) in sorted(groups.items()):
        pre_end=int(np.searchsorted(pre,t,side='left'))
        post_end=int(np.searchsorted(post,t,side='left'))
        positive=float(np.exp(-(t-pre[:pre_end])/synapse['tau_plus']).sum()) if is_post else 0.
        negative=float(np.exp(-(t-post[:post_end])/synapse['tau_minus']).sum()) if is_pre else 0.
        dw=(synapse['a_plus']*(synapse['weight_max']-w)*positive
            -synapse['a_minus']*(w-synapse['weight_min'])*negative)
        new=min(synapse['weight_max'],max(synapse['weight_min'],w+dw))
        updates.append({'time_s':t,'pre':is_pre,'post':is_post,'weight_before':w,'delta':new-w,'weight_after':new})
        w=new;timeline.append((t,w));maximum_pairs+=pre_end*int(is_post)+post_end*int(is_pre)
    event_times=[t for t,w in timeline]
    weights=[w for t,w in timeline]
    result=[weights[bisect.bisect_right(event_times,t+1e-12)-1] for t in sample_times]
    return result,updates,maximum_pairs

def cycles(times,frequency,duration=100.):
    # Frame-aligned periods [k/f,(k+1)/f), matching the stimulus-table frames;
    # all N=duration*f frames are complete, including the initial frame.
    counts=np.zeros(round(duration*frequency),dtype=int)
    for t in times:
        k=int(math.floor(t*frequency))
        if 0<=k<len(counts):counts[k]+=1
    def histogram(a):return {str(int(v)):int((a==v).sum()) for v in np.unique(a)}
    return {'full_frames':len(counts),'spikes_in_frames':int(counts.sum()),'counts_per_frame':histogram(counts),
            'first_10s_counts_per_frame':histogram(counts[:round(10*frequency)]),
            'last_10s_counts_per_frame':histogram(counts[-round(10*frequency):]),
            'first_10_frame_counts':counts[:10].tolist(),'last_10_frame_counts':counts[-10:].tolist()}

def analyze(name,dt,c,label,prefix,provenance,summary):
    with Path(str(prefix)+'_events.csv').open(encoding='utf-8') as stream:
        event_rows=list(csv.DictReader(stream))
    events=[(float(row['time_s']),int(row['node_index_1based'])) for row in event_rows]
    assert all(r['type']=='spike' for r in event_rows)
    assert all(events[i][0]<=events[i+1][0] for i in range(len(events)-1))
    counts=[sum(n==i for _,n in events) for i in (1,2)]
    assert counts==summary['neuron_spikes']
    weights=np.genfromtxt(str(prefix)+'_weights.csv',delimiter=',',names=True)
    expected,updates,pairs=replay(events,c['synapses'][0],weights['time_s'])
    error=float(np.max(np.abs(weights['S1_weight']-expected)))
    assert error<1e-9,(name,'Independent STDP mismatch',error)
    assert abs(float(weights['S1_weight'][-1])-summary['final_synapse_weights'][0])<1e-15
    final=float(weights['S1_weight'][-1]);external=EXTERNAL[name]
    checkpoints={}
    for t in (98.,99.,100.):
        idx=np.flatnonzero(abs(weights['time_s']-t)<1e-9)
        assert len(idx)==1
        checkpoints[str(int(t))]={'time_s':float(weights['time_s'][idx[0]]),'weight':float(weights['S1_weight'][idx[0]]),
            'spikes':[sum(n==i and et<=t+1e-12 for et,n in events) for i in (1,2)]}
    hist=[cycles([t for t,n in events if n==i],c['neurons'][0]['frequency']) for i in (1,2)]
    out={'case':name,'dt_us':dt*1e6,'label':label,'wall_seconds':provenance['wall_seconds'],
         'spike_counts':counts,'final_weight':final,'external_report_weight':external,
         'difference_from_external':final-external,'checkpoints':checkpoints,'cycles':hist,
         'stdp_replay':{'max_sample_weight_abs_error':error,'final_weight':expected[-1],
             'event_groups':len(updates),'strictly_earlier_pairs_enumerated':pairs,'zero_lag_mutual_pairs_excluded':True},
         'config_snapshot_exact':True,'config_sha256':provenance['config_sha256'],'raw_sha256':provenance['raw_sha256']}
    window=(weights['time_s']>=98)&(weights['time_s']<=100)
    out['sampled_98_to_100s_weight_range']=[float(weights['S1_weight'][window].min()),float(weights['S1_weight'][window].max())]
    save(HERE/'raw'/(label+'_analysis.json'),out)
    return out

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--benchmark',action='store_true');ap.add_argument('--skip-run',action='store_true')
    args=ap.parse_args();assert sha(CLI)==EXPECTED_SHA
    scenarios=configurations()
    if args.benchmark:scenarios=scenarios[:1]
    rows=[]
    for name,dt,c in scenarios:
        # Completed benchmark is reused after verifying original hashes/config.
        label=f'{name}_dt{dt*1e6:g}us'.replace('.','p')
        reuse=args.skip_run or (HERE/'raw'/(label+'_run.json')).exists()
        results=run_case(name,dt,c,skip=reuse)
        rows.append(analyze(name,dt,c,*results))
    comparisons=[]
    for name in EXTERNAL:
        pair=[r for r in rows if r['case']==name]
        if len(pair)==2:
            coarse,fine=pair
            comparisons.append({'case':name,'weight_difference_fine_minus_coarse':fine['final_weight']-coarse['final_weight'],
                                'weight_relative_difference_vs_coarse':(fine['final_weight']-coarse['final_weight'])/coarse['final_weight'],
                                'spike_count_difference_fine_minus_coarse':[b-a for a,b in zip(coarse['spike_counts'],fine['spike_counts'])]})
    result={'scope':'Lab5 task1 six pair cases + task2A two pair cases; native multiplicative all-to-all STDP, no triplet rule or external-program equivalence claim.',
       'cli_sha256':sha(CLI),'cli_unchanged':sha(CLI)==EXPECTED_SHA,'frozen_baseline_manifest':load(HERE/'baseline_manifest.json'),
       'user_report':str(USER_REPORT),'user_report_sha256':sha(USER_REPORT),
       'new_runs':len(rows),'distinct_protocols':len({r['case'] for r in rows}),'rows':rows,'step_comparisons':comparisons,
       'initial_condition_choice':'Both nodes V=-65mV,u=bV=-13; independent reset before each run. Exact NeuroNet initial u/elapsed state is not established by the report.',
       'external_limitations':['NeuroNet uses Step0.500ms, current Studio RK4 uses25/12.5us; pulse amplitude and width are held fixed at25x3ms.',
          'NeuroNet Sp.Velocity=.050 can imply transmission delay; Studio has no conduction delays.',
          'Native learning rule is the configured multiplicative all-to-all rule; the proprietary original implementation is not asserted equivalent.',
          'External duration approximately98-101s and stimulus phase at reset are not fully documented; native checkpoints98/99/100s are provided.',
          'Per-cycle count is per full global frame[k/f,(k+1)/f); it describes actual events, not a guarantee one response belongs causally to one pulse.']}
    save(HERE/('benchmark.json' if args.benchmark else 'findings.json'),result)
    print(json.dumps({'runs':len(rows),'results':[(r['label'],r['final_weight'],r['spike_counts']) for r in rows]},indent=2),flush=True)

if __name__=='__main__':main()
