"""Read-only NeuroNet evidence inventory and 500-node native runner bridge.

All generated files stay alongside this script. No original NeuroNet process is
started and no original configuration is modified. Python 3.12 standard library.
"""
from __future__ import annotations
import argparse, copy, csv, hashlib, json, math, re, statistics, subprocess, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[2]
ORIGINAL = Path('C:/Users/N1kit/Downloads/1_NeuroNet')

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
def stats(values):
    return dict(count=len(values), minimum=min(values), maximum=max(values), mean=statistics.mean(values),
                population_sd=statistics.pstdev(values)) if values else dict(count=0)

def parse_net(path):
    text=path.read_text(encoding='cp1251')
    blocks=re.split(r'\nNeuron ID = ',text)
    header=blocks.pop(0)
    nodes=[]; edges=[]
    for block in blocks:
        fields=dict(re.findall(r'^([^=\r\n]+?)\s*=\s*([^\r\n]+)',block,re.M))
        fields={k.strip():v.strip() for k,v in fields.items()}
        node=dict(id=int(block.split()[0]),type=fields['Type'],x=float(fields['CoordinateX']),
                  y=float(fields['CoordinateY']),inputs=int(fields['Number of inputs']),
                  output_deque_size=int(fields['Output_deque size']),
                  stp_label='STDP_YES' in block,
                  parameters={k:float(fields[k]) for k in ['refractory_time','parameter_U','tau_recovery','tau_I',
                      'tau_facility','parameter_a','parameter_b','parameter_c','parameter_d'] if k in fields})
        for source,weight in re.findall(r'^ID\d+ = (\d+)\s+weight\d+ = ([^\r\n]+)',block,re.M):
            edges.append(dict(source=int(source),target=node['id'],weight=float(weight)))
        nodes.append(node)
    assert len(nodes)==int(re.search(r'Number of neurons = (\d+)',header)[1])
    assert sum(n['inputs'] for n in nodes)==len(edges)
    return dict(path=str(path),sha256=sha(path),model_time_ms=float(re.search(r'Model time = ([^\r\n]+)',header)[1]),
                neurons=len(nodes),excitatory=sum(n['type']=='excitory' for n in nodes),
                inhibitory=sum(n['type']=='inhibitory' for n in nodes),edges=len(edges),
                number_of_bursts=int(re.search(r'Number of bursts = (\d+)',header)[1]),
                in_degree=stats([n['inputs'] for n in nodes]),weights=stats([e['weight'] for e in edges]),
                deque_sizes=stats([n['output_deque_size'] for n in nodes]),
                common_parameters=nodes[0]['parameters'],all_parameters_identical=all(n['parameters']==nodes[0]['parameters'] for n in nodes),
                node_details=nodes if len(nodes)<=11 else None),nodes,edges

def evidence():
    paths=[ORIGINAL/'2'/name for name in ['0s.net','0.0005s.net','12962.1s.net','114.15s.net','192.245s.net',
           '32.44s.net','48.13s.net','67.91s.net','ring_N10_w1.net','29.97s.net','network500_k30_initial.net']]
    paths += [ORIGINAL/'1_NeuroNet/3'/name for name in ['29.185s.net','3.net']]
    parsed={str(p):parse_net(p) for p in paths if p.exists()}
    result=dict(snapshots=[v[0] for v in parsed.values()],logs={},delay_matrix={})
    for p in [ORIGINAL/'2'/n for n in ['calculate_0s.log','calculate_32.44s.log','calculate_48.13s.log',
        'wizard_13-32_30-09-26.log','wizard_13-47_30-09-26.log','wizard_13-48_30-09-26.log']]:
        result['logs'][str(p)]=dict(sha256=sha(p),text=p.read_text(encoding='cp1251'))
    p=ORIGINAL/'2/dls_0.0005.txt'
    matrix=[[float(x) for x in line.split()] for line in p.read_text().splitlines() if line.strip()]
    assert len(matrix)==500 and all(len(row)==500 for row in matrix)
    md,nodes,edges=parsed[str(ORIGINAL/'2/0.0005s.net')]
    by_id={n['id']:n for n in nodes}; edge_set={(e['target']-1,e['source']-1) for e in edges}
    positive={(r,c) for r,row in enumerate(matrix) for c,x in enumerate(row) if x>0}
    # Export units are kept literal. Statistical coordinate matching does not
    # establish the simulator's distance conversion or queue indexing contract.
    ratios=[]; edge_values=[]
    for e in edges:
        a,b=by_id[e['source']],by_id[e['target']]
        distance=math.hypot(a['x']-b['x'],a['y']-b['y'])
        delay=matrix[e['target']-1][e['source']-1]
        edge_values.append(delay)
        if distance and delay: ratios.append(delay/distance)
    result['delay_matrix']=dict(path=str(p),sha256=sha(p),rows=500,columns=500,
        nonzero_entries=len(positive),positive_values=stats([matrix[r][c] for r,c in positive]),
        values_on_snapshot_edges=stats(edge_values),positive_entries_outside_snapshot_edges=len(positive-edge_set),
        snapshot_edges_with_zero=len(edge_set-positive),ratio_to_coordinate_distance=stats(ratios),
        snapshot_path=md['path'],units='literal export values; no unit metadata in matrix',
        limitation='This matrix belongs to the 500-node 0.0005s snapshot, not a ring snapshot or the 29.97s network.')
    save(HERE/'neuronet_evidence.json',result)
    print('NeuroNet evidence:',[(Path(p).name,v[0]['neurons'],v[0]['edges']) for p,v in parsed.items()],flush=True)
    print('Delay matrix:',result['delay_matrix'],flush=True)

def bridge(skip_run=False):
    out=HERE/'bridge_500';out.mkdir(exist_ok=True)
    previous=load(out/'comparison.json') if skip_run else None
    base=load(HERE/'populations/raw/k30_seed42_base.json')
    base['duration']=.2;base['sample']=.001
    save(out/'base.json',base)
    edges_path=STUDIO/'lab2/configs/populations/k30_seed42_edges.csv'
    with edges_path.open(newline='') as f:
        edges=[dict(source=int(r['source']),target=int(r['target']),weight=float(r['weight'])) for r in csv.DictReader(f)]
    full=copy.deepcopy(base);full['name']='Lab 2 500-node native bridge';full['neurons']=[];full['synapses']=[]
    full.update(record_all=False,record_neurons=[0,1,2,3],record_synapses=[])
    for i in range(500):
        n=copy.deepcopy(base['neurons'][0]);n.update(name=f'N{i+1}',inhibitory=i>=400,
            px=.05+.9*(i%25)/24,py=.05+.9*(i//25)/19)
        full['neurons'].append(n)
    for edge in edges:
        s=copy.deepcopy(base['synapses'][0]);s.update(edge);full['synapses'].append(s)
    save(out/'config.json',full)
    commands=[(STUDIO/'build/Release/network_studio_population.exe',['--base',str(out/'base.json'),
        '--edges',str(edges_path),'--neurons','500','--output',str(out/'population')]),
        (STUDIO/'build/Release/network_studio_cli.exe',['--config',str(out/'config.json'),'--output',str(out/'core')])]
    if previous:
        assert previous['graph_sha256']==sha(edges_path)
        assert previous['config_sha256']==sha(out/'config.json')
        for run in previous['runs']:assert run['sha256']==sha(run['binary'])
        for name,digest in previous['outputs_sha256'].items():assert sha(out/name)==digest
    result=dict(neurons=500,edges=len(edges),duration_s=.2,dt_s=base['dt'],recorded_neurons=[0,1,2,3],
        graph_sha256=sha(edges_path),config_sha256=sha(out/'config.json'),runs=[])
    for binary,args in commands:
        start=time.perf_counter()
        if not skip_run:
            proc=subprocess.run([str(binary),*args],capture_output=True,text=True,encoding='utf-8',errors='replace')
            (out/(binary.stem+'.log')).write_text(proc.stdout+proc.stderr,encoding='utf-8')
            assert proc.returncode==0,proc.stdout+proc.stderr
        result['runs'].append(previous['runs'][len(result['runs'])] if previous else
            dict(binary=str(binary),sha256=sha(binary),arguments=args,wall_seconds=time.perf_counter()-start))
    pop,core=(load(out/f'{name}_summary.json') for name in ['population','core'])
    assert all(s['complete'] and not s['error'] and abs(s['time_s']-.2)<1e-12 for s in [pop,core])
    assert pop['neuron_spikes']==core['neuron_spikes']
    def events(name):
        with (out/f'{name}_events.csv').open(newline='') as f:
            return list(csv.DictReader(f))
    pe,ce=events('population'),events('core')
    assert pe==ce
    with (out/'population_trace.csv').open(newline='') as f: pt=list(csv.DictReader(f))
    with (out/'core_trace.csv').open(newline='') as f: ct=list(csv.DictReader(f))
    assert len(pt)==len(ct)==201
    max_v=max_u=0.
    for a,b in zip(pt,ct):
        assert float(a['time_s'])==float(b['time_s'])
        for i in range(1,5):
            va,ua=float(a[f'N{i}_v_mV']),float(a[f'N{i}_u_model'])
            vb,ub=float(b[f'N{i}_V_mV']),float(b[f'N{i}_u'])
            assert all(map(math.isfinite,[va,ua,vb,ub]))
            max_v=max(max_v,abs(va-vb));max_u=max(max_u,abs(ua-ub))
    assert max_v<2e-6 and max_u<2e-6
    result.update(passed=True,events=len(pe),sample_rows=len(pt),events_equal=True,
        maximum_sampled_v_error_mV=max_v,maximum_sampled_u_error=max_u,
        limitations='0.2-s numerical bridge of the same 500-node graph, not a new 100-s GUI run; only N1-N4 state columns recorded, all spike events compared.',
        outputs_sha256={p.name:sha(p) for p in out.iterdir() if p.suffix in ['.csv','.json'] and p.name!='comparison.json'})
    save(out/'comparison.json',result)
    print('Bridge:',{k:result[k] for k in ['passed','events','sample_rows','maximum_sampled_v_error_mV','maximum_sampled_u_error']},flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-run',action='store_true')
    args=parser.parse_args()
    evidence();bridge(args.skip_run)
