"""Reproducible numerical validation; all new output is separate from old audits."""
import argparse
import copy
import csv
import hashlib
import importlib.util
import json
import math
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'verification/numerics_20261012'
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT.parent/'tmp/python_deps'))
import numpy as np
import scipy
import reference


def save(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,indent=2,ensure_ascii=False,allow_nan=False)+'\n',encoding='utf-8')


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def native(cli,c,name):
    folder=OUT/'current';folder.mkdir(exist_ok=True)
    config=folder/(name+'.json');prefix=folder/name;save(config,c)
    run=subprocess.run([str(cli),'--config',str(config),'--output',str(prefix)],capture_output=True,text=True)
    (folder/(name+'.log')).write_text(run.stdout+run.stderr,encoding='utf-8')
    if run.returncode:raise RuntimeError(name+': '+run.stderr)
    summary=json.loads(Path(str(prefix)+'_summary.json').read_text(encoding='utf-8'))
    assert summary['complete'] and not summary['error'],summary
    data=np.loadtxt(str(prefix)+'_trace.csv',delimiter=',',skiprows=1,ndmin=2)
    weights=np.loadtxt(str(prefix)+'_weights.csv',delimiter=',',skiprows=1,ndmin=2)
    with Path(str(prefix)+'_events.csv').open(encoding='utf-8',newline='') as f:
        events=[(float(r['time_s']),int(r['node_index_1based'])-1,r['type']!='spike') for r in csv.DictReader(f)]
    return dict(times=data[:,0],values=data[:,1:].T,weights=weights[:,1:].T,events=events,summary=summary)


def replay(c,events,times):
    past=[[] for _ in c['neurons']];weights=np.array([s['weight'] for s in c['synapses']],dtype=float);groups=[]
    for t,i,astro in events:
        if astro:continue
        if not groups or groups[-1][0]!=t:groups.append((t,set()))
        groups[-1][1].add(i)
    result=np.zeros((len(weights),len(times)));index=0
    for column,time in enumerate(times):
        while index<len(groups) and groups[index][0]<=time:
            t,fired=groups[index];weights=reference.learn(c,weights,past,t,fired)
            for i in fired:past[i].append(t)
            index+=1
        result[:,column]=weights
    return result


def compare(c,actual,ref):
    expected,w=ref['sample'](actual['times']);nn=len(c['neurons'])
    timings=[];counts=[]
    for astro,count in [(False,nn),(True,len(c['astrocytes']))]:
        for i in range(count):
            a=[t for t,j,k in actual['events'] if (j,k)==(i,astro)]
            b=[t for t,j,k in ref['events'] if (j,k)==(i,astro)]
            counts.append([len(a),len(b)]);assert len(a)==len(b),(i,astro,a,b)
            timings.extend(abs(x-y) for x,y in zip(a,b))
    error=np.abs(actual['values']-expected)
    voltages=error[:4*nn:4]
    weight_error=float(np.max(np.abs(actual['weights']-w))) if w.size else 0.
    replay_error=float(np.max(np.abs(actual['weights']-replay(c,actual['events'],actual['times'])))) if w.size else 0.
    fractions=[];min_tm=1.
    for i,n in enumerate(c['neurons']):
        if n['model']=='hodgkin_huxley':fractions.extend(actual['values'][4*i+1:4*i+4].ravel())
    for j,s in enumerate(c['synapses']):
        b=4*(nn+j);v=actual['values'][b:b+4];fractions.extend(v[:2].ravel())
        assert v[2:].min()>=-1e-14
        if s['model']=='tsodyks_markram':min_tm=min(min_tm,float(np.min(1-v[1]-v[2])))
    for j in range(len(c['astrocytes'])):
        b=4*(nn+len(c['synapses']))+7*j;v=actual['values'][b:b+7]
        fractions.extend(v[[0,3,4,6]].ravel());assert v[[1,2,5]].min()>=-1e-14
    assert min_tm>=-1e-12 and (not len(fractions) or (min(fractions)>=-1e-12 and max(fractions)<=1+1e-12))
    assert replay_error<1e-10, replay_error
    return dict(dt_us=c['dt']*1e6,counts=counts,max_event_error_ms=1000*max(timings,default=0),
        max_voltage_error_mV=float(voltages.max()),voltage_rms_mV=float(np.sqrt(np.mean(voltages**2))),
        final_voltage_error_mV=float(voltages[:,-1].max()),max_state_scaled_error=float(np.max(error/(1+np.abs(expected)))),
        max_weight_error=weight_error,event_log_replay_max_error=replay_error,min_TM_inactive_pool=min_tm,
        samples=len(actual['times']),native_final=actual['values'][:,-1].tolist(),reference_final=ref['final'].tolist())


def fixture(cli,preset):
    p=OUT/'current'/f'preset_{preset}.json';p.parent.mkdir(exist_ok=True)
    subprocess.run([str(cli),'--preset',str(preset),'--save',str(p)],check=True,capture_output=True)
    return json.loads(p.read_text(encoding='utf-8'))


def scenarios(cli):
    single=fixture(cli,4);single.update(duration=.2,sample=.0001)
    single['neurons'][0].update(current=10,amplitude=0)
    rs=copy.deepcopy(single);fs=copy.deepcopy(single);fs['neurons'][0].update(izh_a=.1,izh_d=2)
    pulse=copy.deepcopy(rs);pulse.update(duration=.12);pulse['neurons'][0].update(current=0,amplitude=30,start=.010101,stop=.12,width=.001013,frequency=20)
    hh=fixture(cli,3);hh.update(duration=.05,sample=.0001);hh['neurons'][0].update(current=10,amplitude=0)
    coupled=fixture(cli,5);coupled.update(duration=.1,sample=.0001)
    coupled['neurons'][0].update(current=10,amplitude=0);coupled['neurons'][1].update(current=3,amplitude=0)
    astro=copy.deepcopy(fixture(cli,0)['astrocytes'][0]);astro.update(ip3=.8,calcium=.19,threshold=.2)
    coupled['astrocytes']=[astro];coupled['synapses'][0].update(astro=0,gain=4,spill=.05)
    cases=dict(RS=rs,FS=fs,HH=hh,pulse=pulse,small_coupled=coupled)
    for rule in ['additive','multiplicative','triplet']:
        stdp=fixture(cli,6);stdp.update(duration=.1,sample=.0001)
        stdp['neurons'][0].update(current=10,amplitude=0);stdp['neurons'][1].update(current=14,amplitude=0)
        stdp['synapses'][0].update(stdp_rule=rule,gain=.15)
        cases['STDP_'+rule]=stdp
    fast=copy.deepcopy(coupled);fast.update(duration=.002,sample=50e-6)
    fast['neurons'][0].update(voltage=29,current=0);fast['neurons'][1].update(current=0)
    fast['astrocytes'][0].update(calcium=.19998)
    fast['parameters'].update(Omega_Y=200000,Omega_N=50000,O_N=5000,Omega_G=100000,O_G=5000,Omega_S=20000)
    cases['fast_coupled']=fast
    return cases


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case',action='append');args=parser.parse_args()
    cli=ROOT/'build/Release/network_studio_cli.exe';cases=scenarios(cli);rows={};cross=[]
    for name,c in cases.items():
        if args.case and name not in args.case:continue
        save(OUT/'current'/f'{name}_reference_config.json',c)
        print('REFERENCE',name,flush=True);ref=reference.solve(c)
        save(OUT/'current'/f'{name}_reference_events.json',ref['events'])
        rows[name]=[]
        for dt in [50e-6,25e-6,12.5e-6]:
            run=copy.deepcopy(c);run['dt']=dt
            actual=native(cli,run,f'{name}_{dt*1e6:g}us')
            row=compare(run,actual,ref);rows[name].append(row)
            assert row['max_event_error_ms']<1e-5 and row['max_voltage_error_mV']<1e-3,row
            assert row['max_weight_error']<1e-7,row
            print(name,dt*1e6,'us; timing ms',row['max_event_error_ms'],'voltage',row['max_voltage_error_mV'],'weights',row['max_weight_error'],flush=True)
        if name!='fast_coupled':
            assert rows[name][-1]['max_event_error_ms']<rows[name][0]['max_event_error_ms'],(name,'event convergence')
            assert rows[name][-1]['voltage_rms_mV']<.6*rows[name][0]['voltage_rms_mV'],(name,'trajectory convergence')
        if name in ['FS','HH','fast_coupled']:
            alt=reference.solve(c,method='Radau');times=np.linspace(0,c['duration'],501)
            a,wa=ref['sample'](times);b,wb=alt['sample'](times)
            assert len(ref['events'])==len(alt['events'])
            cross.append(dict(case=name,max_event_difference_ms=1000*max((abs(a[0]-b[0]) for a,b in zip(ref['events'],alt['events'])),default=0),
                              max_state_scaled_difference=float(np.max(abs(a-b)/(1+abs(a))))))
            assert cross[-1]['max_event_difference_ms']<1e-5 and cross[-1]['max_state_scaled_difference']<1e-6,cross[-1]
        save(OUT/'current'/'convergence_partial.json',dict(rows=rows,crosschecks=cross))
    result=dict(cli_path=str(cli),cli_sha256=digest(cli),python=sys.version,numpy=np.__version__,scipy=scipy.__version__,
        reference=dict(method='DOP853',rtol=2e-11,atol=2e-13,max_step_s=50e-6,discontinuities='explicit input breakpoints and terminal threshold/reset events'),
        rows=rows,crosschecks=cross)
    save(OUT/'current'/('convergence.json' if not args.case else 'selected_convergence.json'),result)
    print('Validation completed.',flush=True)


if __name__=='__main__':main()
