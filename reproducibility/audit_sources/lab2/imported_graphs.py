"""Restart the two saved NeuroNet Lab 2 graphs in the current native TM model.

Only topology, E/I labels and serialized weights are imported. This deliberately
does not continue NeuroNet state or claim to emulate its delays/solver/noise.
"""
from __future__ import annotations
import argparse, concurrent.futures, copy, csv, importlib.util, json, os, subprocess, sys, time
from pathlib import Path
sys.dont_write_bytecode=True
from details import HERE, STUDIO, ORIGINAL, parse_net, sha, load, save

def module(name):
    spec=importlib.util.spec_from_file_location('source_'+name,STUDIO/'lab2'/f'{name}.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-deps',type=Path,required=True)
    parser.add_argument('--skip-run',action='store_true')
    args=parser.parse_args();os.environ['MPLCONFIGDIR']=str(HERE/'mpl')
    out=HERE/'imported_graphs';configs=out/'configs/populations';results=out/'results/populations';raw=results/'raw'
    for p in [configs,raw]:p.mkdir(parents=True,exist_ok=True)
    scenarios={};provenance={};sources={}
    pop=module('populations');pop.imports(args.python_deps);pop.CONFIGS=configs;pop.RESULTS=results;pop.RAW=raw
    checker=module('check_metrics')
    for label,path in [('nn_k30',ORIGINAL/'2/29.97s.net'),('nn_k15',ORIGINAL/'1_NeuroNet/3/29.185s.net')]:
        metadata,nodes,edges=parse_net(path)
        assert len(nodes)==500 and metadata['excitatory']==400 and metadata['inhibitory']==100
        assert metadata['all_parameters_identical']
        nodes.sort(key=lambda n:(n['type']!='excitory',n['id']))
        index={n['id']:i for i,n in enumerate(nodes)};types={n['id']:n['type'] for n in nodes}
        rows=[dict(source=index[e['source']],target=index[e['target']],
                   weight=e['weight']*(-1 if types[e['source']]=='inhibitory' else 1)) for e in edges]
        assert all(e['weight']>=0 for e in edges)
        with (configs/f'{label}_edges.csv').open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=['source','target','weight']);writer.writeheader();writer.writerows(rows)
        base=load(HERE/'populations/raw/k30_seed42_base.json')
        base.update(name=f'Lab2 imported graph control {label}',duration=30.,sample=.01)
        save(configs/f'{label}_base.json',base)
        scenarios[label]=dict(base=base,graph=label,role='imported_neuronet_graph_control')
        sources[label]=dict(metadata=metadata,index_mapping={str(n['id']):index[n['id']] for n in nodes},
            transformation='E/I source type determines signed weights; map IDs to E first / I second. Preserve serialized topology and magnitudes. Reset RS V=-65,u=-13 and TM pools; seed42 sample-held noise3; RK4 dt25us; no conduction delay; no STDP.',
            source_sha256=sha(path),imported_edges_sha256=sha(configs/f'{label}_edges.csv'))
    exe=STUDIO/'build/Release/network_studio_population.exe'
    def run(name):
        base,edges=configs/f'{name}_base.json',configs/f'{name}_edges.csv'
        command=[str(exe),'--base',str(base),'--edges',str(edges),'--neurons','500','--output',str(raw/name)]
        begin=time.perf_counter()
        if not args.skip_run:
            proc=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace')
            (raw/f'{name}.log').write_text(proc.stdout+proc.stderr,encoding='utf-8')
            assert proc.returncode==0,proc.stdout+proc.stderr
        result=dict(command=command,wall_seconds=time.perf_counter()-begin,executable_sha256=sha(exe),
            base_sha256=sha(base),edges_sha256=sha(edges),outputs={s:sha(raw/f'{name}_{s}')
            for s in ['base.json','events.csv','trace.csv','summary.json']})
        print(name,'complete',flush=True)
        return name,result
    if args.skip_run:
        previous=load(results/'run_provenance.json')
        for name,p in previous.items():
            assert p['executable_sha256']==sha(exe)
            for s,h in p['outputs'].items():assert sha(raw/f'{name}_{s}')==h
        provenance=previous
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            for name,p in pool.map(run,scenarios):provenance[name]=p
    save(results/'run_provenance.json',provenance)
    metrics=pop.analyze(scenarios,list(scenarios))
    independent={name:checker.run_check(name,spec,out,provenance[name],metrics[name]) for name,spec in scenarios.items()}
    save(results/'independent_metrics.json',independent)
    save(out/'source_provenance.json',sources)
    print(json.dumps({name:{k:m[k] for k in ['spikes','mean_rate_hz','burst_rate_hz','max_population_rate_hz']} for name,m in metrics.items()},indent=2),flush=True)

if __name__=='__main__':main()
