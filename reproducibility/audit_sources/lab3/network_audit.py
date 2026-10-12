"""Import the original Lab3 network into the current zero-delay RK4 core.

This is a documented model transfer, not execution of original NeuroNet.
The original graph/weights/types/states/TM/stimulator table are retained.
Transport queues, Euler sampling and the output timer are not reproduced.
"""
from __future__ import annotations
import argparse, bisect, concurrent.futures, copy, csv, hashlib, json, math, re, statistics, sys, time
from pathlib import Path
sys.dont_write_bytecode=True
import audit as a

SOURCE=a.extract.DEFAULT_SOURCE
def field(block,key):
    m=re.search(r'^'+re.escape(key)+r' = ([^\r\n]+)',block,re.M)
    if not m: raise ValueError(key)
    return m.group(1).strip()
def num(block,key): return float(field(block,key))

def import_network(source,duration=30.):
    text=source.read_text(encoding='cp1251')
    matches=list(re.finditer(r'^Neuron ID = (\d+)\s*\n(.*?)(?=^Neuron ID = |^Number of stimulators = )',text,re.M|re.S))
    assert len(matches)==int(num(text,'Number of neurons'))==500
    raw={int(m.group(1)):m.group(2) for m in matches}; assert sorted(raw)==list(range(1,501))
    original_stim=a.extract.stimulus(text)
    period=original_stim['frame_duration_ms']/1000;table=original_stim['interval_table']
    assert len(table)==3 and table[0]['voltage_setting']==table[2]['voltage_setting']==0
    begin=table[0]['end_time_ms']/1000; width=(table[1]['end_time_ms']-table[0]['end_time_ms'])/1000
    assert table[2]['end_time_ms']/1000==period
    targets=original_stim['target_neuron_ids_1based']
    c=a.compat.canonical_config(a.read(a.LAB/'configs/f100.json'))
    ntemplate=copy.deepcopy(c['neurons'][0]);stemplate=copy.deepcopy(c['synapses'][0])
    c.update(name='Original NeuroNet graph / current RK4 zero-delay / '+source.stem,duration=duration,dt=.000025,
             sample=.001,noise_sigma=0.,neurons=[],synapses=[],record_all=False,record_neurons=[i-1 for i in targets],record_synapses=[])
    coords={i:(num(b,'CoordinateX'),num(b,'CoordinateY')) for i,b in raw.items()}
    lo=[min(p[k] for p in coords.values()) for k in [0,1]];hi=[max(p[k] for p in coords.values()) for k in [0,1]]
    edges=[]; deque_sizes=[]
    for i,b in raw.items():
        assert field(b,'Class')=='Izh'; inhib=field(b,'Type')=='inhibitory'
        assert field(b,'Type') in ['inhibitory','excitory']
        assert num(b,'parameter_x')==20 and num(b,'Output')==num(b,'parameter_z')==num(b,'parameter_u')==0
        assert num(b,'Time_cur_spike')==num(b,'Is_spike')==0
        dq=int(num(b,'Output_deque size'));deque_sizes.append(dq)
        assert all(num(b,f'o_d{k}')==0 for k in range(dq))
        n=copy.deepcopy(ntemplate)
        n.update(name=f'Original {i}',inhibitory=inhib,px=(coords[i][0]-lo[0])/(hi[0]-lo[0]),py=(coords[i][1]-lo[1])/(hi[1]-lo[1]),
                 voltage=num(b,'E_m'),izh_u=num(b,'variable_U_e'),equilibrium_u=False,current=0.,amplitude=table[1]['voltage_setting'] if i in targets else 0.,
                 start=begin,stop=duration,width=width,frequency=1/period,individual_noise=False,noise_sigma=0.)
        for key in ['a','b','c','d']: n['izh_'+key]=num(b,'parameter_'+key)
        c['neurons'].append(n)
        inputs=re.findall(r'^ID(\d+) = (\d+)\s+weight\1 = ([\d.eE+-]+)',b,re.M)
        assert len(inputs)==int(num(b,'Number of inputs'))
        for index,source_id,weight in inputs:
            source_id=int(source_id);w=float(weight); assert 1<=source_id<=500 and w>=0
            edges.append((source_id,i,w))
    assert len(edges)==len({(s,t) for s,t,w in edges}) and all(s!=t for s,t,w in edges)
    for source_id,target_id,w in edges:
        b=raw[source_id];s=copy.deepcopy(stemplate)
        s.update(source=source_id-1,target=target_id-1,weight=-w if c['neurons'][source_id-1]['inhibitory'] else w,
                 reversal=-80 if c['neurons'][source_id-1]['inhibitory'] else 0,gain=20.,release=num(b,'parameter_U'),
                 tau_syn=num(b,'tau_I')/1000,tau_d=num(b,'tau_recovery')/1000,tau_f=num(b,'tau_facility')/1000,
                 stdp=False,spill=0.,astro=-1)
        if source_id in targets and not any(c['synapses'][k]['source']==source_id-1 for k in c['record_synapses']):
            c['record_synapses'].append(len(c['synapses']))
        c['synapses'].append(s)
    assert len(c['record_synapses'])==len(targets)
    graphsha=hashlib.sha256(json.dumps(edges,separators=(',',':')).encode()).hexdigest()
    info=dict(source=str(source),source_sha256=a.sha(source),neuron_count=500,edge_count=len(edges),
              inhibitory_count=sum(n['inhibitory'] for n in c['neurons']),graph_list_sha256=graphsha,
              initial_voltage_values=sorted({n['voltage'] for n in c['neurons']}),initial_recovery_values=sorted({n['izh_u'] for n in c['neurons']}),
              degree_mean=len(edges)/500,absolute_weight_mean=statistics.mean(w for s,t,w in edges),
              original_stimulus=original_stim,output_deque_size_range=[min(deque_sizes),max(deque_sizes)],coordinates=coords,
              unsupported_original_fields=['Output_deque transport history / spike velocity','NeuroNet coarse Euler integration','Output spike timer'])
    return c,info

def hull_area(points):
    p=sorted(set(points))
    if len(p)<3:return 0.
    def cross(o,a,b):return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lower=[]
    for q in p:
        while len(lower)>=2 and cross(lower[-2],lower[-1],q)<=0:lower.pop()
        lower.append(q)
    upper=[]
    for q in reversed(p):
        while len(upper)>=2 and cross(upper[-2],upper[-1],q)<=0:upper.pop()
        upper.append(q)
    h=lower[:-1]+upper[:-1]
    return abs(sum(x*v-y*u for (x,y),(u,v) in zip(h,h[1:]+h[:1])))/2

def analyze(name,c,info,elapsed):
    summary=a.read(a.rawpath(name,'summary.json')); assert summary['complete'] and not summary['error']
    events={i:[] for i in range(1,501)}
    with a.rawpath(name,'events.csv').open(encoding='utf-8',newline='') as f:
        for r in csv.DictReader(f):
            assert r['type']=='spike'; events[int(r['node_index_1based'])].append(float(r['time_s']))
    assert [len(events[i]) for i in range(1,501)]==summary['neuron_spikes']
    assert summary['final_synapse_weights']==[s['weight'] for s in c['synapses']]
    frequency=c['neurons'][0]['frequency'];start=c['neurons'][0]['start'];targets=info['original_stimulus']['target_neuron_ids_1based']
    left=10.;right=c['duration'];assert right>left
    active=[i for i,t in events.items() if bisect.bisect_left(t,right)>bisect.bisect_left(t,left)]
    counts=[bisect.bisect_left(events[i],right)-bisect.bisect_left(events[i],left) for i in range(1,501)]
    first=math.ceil((left-start)*frequency);last=math.floor((right-start)*frequency)
    recruited=[]
    for k in range(first,last):
        l=start+k/frequency;r=start+(k+1)/frequency
        recruited.append(sum(bisect.bisect_left(t,r)>bisect.bisect_left(t,l) for t in events.values()))
    syn_i=next(i for i in c['record_synapses'] if c['synapses'][i]['source']==452)
    responses=a.tm.tm_responses(events[453]);steady=[r for r in responses if left<=r['time_s']<right]
    assert steady
    period_peaks=[]
    for k in range(first,last):
        values=[q['peak'] for q in responses if start+k/frequency<=q['time_s']<start+(k+1)/frequency]
        if values:period_peaks.append(max(values))
    with a.rawpath(name,'trace.csv').open(encoding='utf-8',newline='') as f:
        rows=[(float(r['time_s']),*[float(r[f'S{syn_i+1}_{k}']) for k in ['u','x','y']]) for r in csv.DictReader(f)]
    ref=a.tm.state_at(events[453],[r[0] for r in rows])
    errors={key:max(abs(r[j+1]-q[key]) for r,q in zip(rows,ref)) for j,key in enumerate(['u','x','y'])}
    assert max(errors.values())<1e-8
    coords={int(i):tuple(p) for i,p in info['coordinates'].items()}
    return dict(name=name,frequency_hz=frequency,dt_us=c['dt']*1e6,duration_s=right,wall_s=elapsed,analysis_window_s=[left,right],
                source_graph_sha256=info['graph_list_sha256'],neuron_spikes=summary['neuron_spikes'],
                active_neuron_count=len(active),active_nonstimulated_count=len(set(active)-set(targets)),active_ids_1based=active,
                total_steady_spikes=sum(counts),mean_population_rate_hz=sum(counts)/(500*(right-left)),
                active_convex_hull_fraction=hull_area([coords[i] for i in active])/hull_area(coords.values()),
                complete_period_recruited=dict(periods=len(recruited),min=min(recruited) if recruited else None,max=max(recruited) if recruited else None,
                                               mean=statistics.mean(recruited) if recruited else None),
                neuron453=dict(spike_rate_hz=counts[452]/(right-left),output_peak_mean=statistics.mean(r['peak'] for r in steady),
                               output_peak_min=min(r['peak'] for r in steady),output_peak_max=max(r['peak'] for r in steady),
                               stimulus_period_output_peak_mean=statistics.mean(period_peaks),
                               stimulus_period_output_peak_min=min(period_peaks),stimulus_period_output_peak_max=max(period_peaks),
                               responding_complete_periods=len(period_peaks),
                               exact_tm_max_abs_errors=errors),csv_sha256={k:a.sha(a.rawpath(name,k+'.csv')) for k in ['trace','events','weights']})

def run_network(name,c,skip):
    _,summary,elapsed=a.run(name,c,skip)
    print('NETWORK RUN DONE',name,'wall',elapsed,'spikes',sum(summary['neuron_spikes']),flush=True)
    return elapsed

def main():
    p=argparse.ArgumentParser();p.add_argument('--skip-run',action='store_true');p.add_argument('--benchmark-only',action='store_true');p.add_argument('--short-sensitivity',action='store_true');args=p.parse_args()
    result=dict(cli_sha256=a.sha(a.CLI),model_scope=__doc__,imports=[],runs=[])
    previous_wall={r['name']:r['wall_s'] for r in a.read(a.HERE/'network_findings.json')['runs']} if (a.HERE/'network_findings.json').exists() else {}
    scenarios=[]
    for fname in ['net1_0.1Hz.net','net1_1Hz.net','net1_100Hz.net']:
        c,info=import_network(SOURCE/fname);result['imports'].append(info)
        name='network_'+fname.removesuffix('.net').replace('.','p');scenarios.append((name,c,info))
        a.save(a.HERE/'configs'/f'{name}.json',c)
    assert len({i['graph_list_sha256'] for i in result['imports']})==1
    a.save(a.HERE/'network_import.json',result)
    if args.short_sensitivity:
        base=copy.deepcopy(scenarios[0][1]);base['duration']=.2
        for n in base['neurons']:n['stop']=.2
        rows=[]
        for div,suffix in [(1,''),(2,'_fine'),(4,'_quarter'),(8,'_eighth'),(16,'_sixteenth')]:
            c=copy.deepcopy(base);c['dt']/=div;name='network_benchmark'+suffix
            elapsed=run_network(name,c,args.skip_run)
            s=a.read(a.rawpath(name,'summary.json'))
            events={i:[] for i in range(1,501)}
            with a.rawpath(name,'events.csv').open(encoding='utf-8',newline='') as f:
                for r in csv.DictReader(f):events[int(r['node_index_1based'])].append(float(r['time_s']))
            rows.append(dict(name=name,dt_us=c['dt']*1e6,total_spikes=sum(s['neuron_spikes']),counts=s['neuron_spikes'],
                             active_neurons=sum(x>0 for x in s['neuron_spikes']),events=events,wall_s=elapsed))
        ref=rows[-1]
        for row in rows:
            changes=[abs(x-y) for x,y in zip(row['counts'],ref['counts'])]
            first=[abs(row['events'][i][0]-ref['events'][i][0])*1000 for i in range(1,501) if row['events'][i] and ref['events'][i]]
            row.update(relative_total_spike_difference=100*(row['total_spikes']/ref['total_spikes']-1),
                       neurons_with_different_spike_count=sum(d!=0 for d in changes),
                       neuron_count_vector_l1=sum(changes),first_spike_abs_difference_ms_max=max(first),
                       first_spike_abs_difference_ms_median=statistics.median(first))
        for row in rows:del row['events']
        a.save(a.HERE/'network_step_sensitivity.json',dict(duration_s=.2,reference_dt_us=1.5625,rows=rows,
                                                         cli_sha256=a.sha(a.CLI),script_sha256=a.sha(__file__)))
        return
    if args.benchmark_only:
        name,c,_=scenarios[0];c=copy.deepcopy(c);c['duration']=.2
        for n in c['neurons']:n['stop']=.2
        elapsed=run_network('network_benchmark',c,False)
        a.save(a.HERE/'network_benchmark.json',dict(model_duration_s=.2,wall_s=elapsed,estimated_30s_wall_s=elapsed*150))
        return
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures={pool.submit(run_network,name,c,args.skip_run):(name,c,info) for name,c,info in scenarios}
        for future in concurrent.futures.as_completed(futures):
            name,c,info=futures[future];elapsed=future.result()
            if elapsed is None:elapsed=previous_wall.get(name)
            row=analyze(name,c,info,elapsed)
            result['runs'].append(row);a.save(a.HERE/'network_findings.json',result)
    result['runs'].sort(key=lambda row:row['frequency_hz'])
    result['script_sha256']=a.sha(__file__)
    assert a.sha(a.CLI)==result['cli_sha256']
    a.save(a.HERE/'network_findings.json',result)
    print('NETWORK AUDIT COMPLETE',flush=True)

if __name__=='__main__':main()
