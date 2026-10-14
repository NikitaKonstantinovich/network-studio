"""Analyze new laboratory replays without modifying old reports or raw data."""
from __future__ import annotations
import argparse, bisect, collections, csv, importlib.util, math, statistics
from pathlib import Path
import numpy as np
from scipy.signal import find_peaks
from replay import ROOT, read, save, sha, suff, load_events

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
tm=module('audit_tm',ROOT/'lab3/reference_tm.py')
stdp=module('audit_stdp',ROOT/'reproducibility/lab5/updated/run_updated.py')

def basic(prefix,c,count=None):
    t,n=load_events(prefix);start=1. if c['duration']>=5 else 0.
    neurons=[]
    for i in range(1,(count or len(c['neurons']))+1):
        x=t[n==i];late=x[(x>=start)&(x<c['duration'])];isi=np.diff(late)
        neurons.append(dict(spikes=len(x),rate_hz=len(late)/(c['duration']-start),
            isi_rate_hz=float(1/isi.mean()) if len(isi) else None,isi_cv=float(isi.std()/isi.mean()) if len(isi) else None))
    return neurons

def ring(prefix,c):
    t,n=load_events(prefix);size=len(c['neurons']);late=(t>=4)&(t<5);tl,nl=t[late],n[late]
    counts=np.bincount(nl,minlength=size+1)[1:];after=np.bincount(n[t>.101],minlength=size+1)[1:]
    order=float(np.mean((np.diff(tl)>0)&(nl[1:]==nl[:-1]%size+1))) if len(tl)>1 else None
    sustained=bool((counts>=2).all() and order is not None and order>=.95 and len(t) and t[-1]>=4.9 and after.all())
    isi=np.diff(tl[nl==1])
    return dict(spikes=len(t),counts=np.bincount(n,minlength=size+1)[1:].tolist(),late_counts=counts.tolist(),
        sustained=sustained,late_order=order,late_rate_hz=int(counts[0]),isi_rate_hz=float(1/isi.mean()) if sustained and len(isi) else None)

def peaks(rates,distance):
    candidates,_=find_peaks(rates,height=np.nextafter(5.,np.inf));blocked=np.zeros(len(rates),bool);selected=[]
    for p in sorted(candidates,key=lambda p:(-rates[p],p)):
        if not blocked[p]:selected.append(p);blocked[max(0,p-distance+1):p+distance]=True
    return np.asarray(sorted(selected),dtype=int)

def population(prefix,c):
    t,n=load_events(prefix);ticks=np.arange(round(c['duration']*1000)+1)/1000
    rates=(np.searchsorted(t,ticks+1e-12,'right')-np.searchsorted(t,ticks-.05+1e-12,'right'))/25
    burst=ticks[peaks(rates,50)];end=min(30.,c['duration']);span=end-5
    return dict(spikes=len(t),active_neurons=len(set(n)),mean_rate_hz=float(np.sum((t>5)&(t<=end))/(500*span)),
        burst_rate_hz=float(np.sum((burst>5)&(burst<=end))/span),window_s=[5,end],
        late_rate_hz=float(np.sum(t>5)/(500*(c['duration']-5))))

def tm_case(prefix,c,synapse_index=0,source=0,window=None):
    t,n=load_events(prefix);ev=t[n==source+1];s=c['synapses'][synapse_index]
    kw=dict(U=s['release'],tau_fac=s['tau_f'],tau_rec=s['tau_d'],tau_syn=s['tau_syn'],gain=s['gain'])
    responses=tm.tm_responses(ev,**kw);f=c['neurons'][source]['frequency'];start=c['neurons'][source]['start']
    if not f:return dict(spikes=len(ev))
    first=max(2,math.ceil(5*f)) if window is None else math.ceil((window[0]-start)*f)
    last=math.floor((c['duration']-start)*f+1e-8);counts=[];peak=[]
    for k in range(first,last):
        selected=[r for r in responses if start+k/f<=r['time_s']<start+(k+1)/f]
        counts.append(len(selected))
        if selected:peak.append(max(r['peak'] for r in selected))
    result=dict(spikes=len(ev),frequency_hz=f,spike_rate_hz=statistics.mean(counts)*f if counts else None,
        peak_mean=statistics.mean(peak) if peak else None,spikes_per_period_min=min(counts,default=None),spikes_per_period_max=max(counts,default=None))
    data=np.genfromtxt(suff(prefix,'trace.csv'),delimiter=',',names=True)
    ref=tm.state_at(ev,data['time_s'],**kw)
    errors={key:float(max(abs(data[f'S{synapse_index+1}_{key}']-np.asarray([r[key] for r in ref])))) for key in ('u','x','y')}
    result['tm_max_abs_errors']=errors
    # Event-driven resource recurrence is independent of neuron integration.
    assert max(errors.values())<1e-8,(prefix,errors)
    return result

def replay_weights(prefix,c):
    t,n=load_events(prefix);data=np.genfromtxt(suff(prefix,'weights.csv'),delimiter=',',names=True)
    errors=[]
    for j,s in enumerate(c['synapses']):
        if not s.get('stdp',False):continue
        # Direct sums over all earlier events, independent of native traces.
        seq=[]
        for et,node in zip(t,n):
            if node==s['source']+1:seq.append((float(et),1))
            if node==s['target']+1:seq.append((float(et),2))
        expected,_=stdp.direct_replay(sorted(seq),dict(s,tau_slow=s.get('tau_slow',.1)),data['time_s'])
        errors.append(float(np.max(abs(data[f'S{j+1}_weight']-expected))))
    assert max(errors,default=0)<1e-9,(prefix,errors)
    return errors

def analyze(out):
    records=[read(p) for p in out.glob('*/runs/*.json')]
    result=dict(groups={},lab1={},rings={},populations={},lab3={},lab3_network={},lab4={},sources={})
    for r in records:
        if not r.get('complete'):continue
        j=r['input'];g=j['group'];name=j['name'];c=read(j['config']);prefix=out/g/'raw'/name;old=j['old_prefix']
        group=result['groups'].setdefault(g,dict(completed=0,same_spike_counts=0,csv_identical=0,csv_compared=0))
        group['completed']+=1;group['same_spike_counts']+=int(r['comparison']['events']['counts_equal'])
        for x in r['comparison'].values():group['csv_compared']+=1;group['csv_identical']+=int(x['byte_identical'])
        if g.startswith('lab1'):result['lab1'][name]=dict(old=basic(old,c),new=basic(prefix,c),event_comparison=r['comparison']['events'])
        elif g in ('lab2_rings','lab2_ring11'):result['rings'][name]=dict(old=ring(old,c),new=ring(prefix,c))
        elif g in ('lab2_populations','lab2_imported'):result['populations'][name]=dict(old=population(old,c),new=population(prefix,c))
        elif g in ('lab3','lab3_calibrated'):
            result['lab3'][name]=dict(old=tm_case(old,c),new=tm_case(prefix,c),group=g)
        elif g=='lab3_network':
            def metric(p):
                t,n=load_events(p);keep=(t>=10)&(t<30);sub=n[keep]
                source=452;syn=next(i for i in c['record_synapses'] if c['synapses'][i]['source']==source)
                return dict(active_neurons=len(set(sub)),steady_spikes=len(sub),mean_rate_hz=len(sub)/10000,
                    neuron453=tm_case(p,c,syn,source,(10,30)))
            result['lab3_network'][name]=dict(old=metric(old),new=metric(prefix))
        elif g.startswith('lab4_'):
            a,b=r['old_summary'],r['summary'];errors=replay_weights(prefix,c)
            result['lab4'][name]=dict(group=g,old_weights=a['final_synapse_weights'],new_weights=b['final_synapse_weights'],
                old_spikes=a['neuron_spikes'],new_spikes=b['neuron_spikes'],replay_errors=errors,
                max_weight_change=max(abs(x-y) for x,y in zip(a['final_synapse_weights'],b['final_synapse_weights'])),dt=c['dt'])
    result['large_network_convergence']=[]
    benchmarks=[r for r in records if r['input']['group']=='lab3_network_convergence']
    if len(benchmarks)==5:
        finest=min(benchmarks,key=lambda r:r['input']['dt'])
        def prefix_of(r):return out/r['input']['group']/'raw'/r['input']['name']
        rt,rn=load_events(prefix_of(finest))
        reference=np.genfromtxt(suff(prefix_of(finest),'trace.csv'),delimiter=',',names=True)
        columns=[n for n in reference.dtype.names if n.endswith('_V_mV')]
        assert columns,reference.dtype.names
        for r in sorted(benchmarks,key=lambda r:r['input']['dt'],reverse=True):
            t,n=load_events(prefix_of(r));equal=True;error=0.
            for node in range(1,501):
                x,y=t[n==node],rt[rn==node]
                if len(x)!=len(y):equal=False
                elif len(x):error=max(error,float(max(abs(x-y))))
            trace=np.genfromtxt(suff(prefix_of(r),'trace.csv'),delimiter=',',names=True)
            assert np.array_equal(trace['time_s'],reference['time_s'])
            delta=np.concatenate([trace[n]-reference[n] for n in columns])
            result['large_network_convergence'].append(dict(dt_us=r['input']['dt']*1e6,
                spikes=len(t),same_per_neuron_counts_as_finest=equal,
                max_event_difference_ms=1000*error if equal else None,
                recorded_voltage_rms_difference_mV=float(np.sqrt(np.mean(delta*delta))),
                reference_dt_us=finest['input']['dt']*1e6))
    for path in [ROOT/'lab3/reference_tm.py',ROOT/'reproducibility/lab5/updated/run_updated.py',Path(__file__)]:result['sources'][str(path)]=sha(path)
    save(out/'analysis.json',result)
    print('ANALYZED',result['groups'],flush=True)
    return result

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True)
    analyze(ap.parse_args().output.resolve())
