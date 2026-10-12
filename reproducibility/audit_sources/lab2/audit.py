"""Replay the saved Lab 2 protocol without modifying its inputs or results.

Uses current Release binaries and the existing analysis functions with output
directories redirected here. Two native processes at most. Run with Python -B.
"""
from __future__ import annotations
import argparse, concurrent.futures, copy, csv, hashlib, importlib.util, json
import math, os, statistics, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[2]
SOURCE = STUDIO / 'lab2'
CLI = STUDIO / 'build/Release/network_studio_cli.exe'
POP = STUDIO / 'build/Release/network_studio_population.exe'

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''): h.update(block)
    return h.hexdigest()

def load(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def save(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)+'\n', encoding='utf-8')

def module(name):
    spec = importlib.util.spec_from_file_location('audit_'+name, SOURCE/(name+'.py'))
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

def replay(task):
    kind,name,command,inputs,suffixes,old_dir = task
    prefix = HERE/kind/'raw'/name; prefix.parent.mkdir(parents=True,exist_ok=True)
    start = time.perf_counter()
    done = subprocess.run(command+['--output',str(prefix)], capture_output=True,text=True,encoding='utf-8',errors='replace')
    Path(str(prefix)+'_cli.log').write_text(done.stdout+done.stderr,encoding='utf-8')
    if done.returncode: raise RuntimeError(name+': '+done.stdout+done.stderr)
    outputs = {s:sha(str(prefix)+'_'+s) for s in suffixes}
    csv_comparisons = {s:dict(current_sha256=outputs[s], previous_sha256=sha(old_dir/(name+'_'+s)),
                             byte_identical=outputs[s]==sha(old_dir/(name+'_'+s)))
                       for s in suffixes if s.endswith('.csv')}
    return dict(kind=kind,name=name,wall_seconds=round(time.perf_counter()-start,3),command=command+['--output',str(prefix)],
                inputs_sha256={str(p):sha(p) for p in inputs},outputs_sha256=outputs,csv_comparisons=csv_comparisons)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-deps',type=Path,required=True)
    parser.add_argument('--skip-run',action='store_true')
    args=parser.parse_args();sys.dont_write_bytecode=True
    sys.path.insert(0,str(args.python_deps));os.environ['MPLCONFIGDIR']=str(HERE/'mpl')
    rings,pops=module('rings'),module('populations')
    rings.RAW=HERE/'rings/raw';rings.RESULTS=HERE/'rings';rings.FIGURES=HERE/'figures'
    pops.RAW=HERE/'populations/raw';pops.RESULTS=HERE/'populations';pops.FIGURES=HERE/'figures'
    pops.imports(args.python_deps)
    old_rings=load(SOURCE/'results/rings/metrics.json')
    old_pops=load(SOURCE/'results/populations/metrics.json')
    manifest=load(SOURCE/'results/populations/manifest.json')
    scenarios={name:dict(base=load(SOURCE/'configs/populations'/f'{name}_base.json'),**entry)
               for name,entry in manifest['scenarios'].items()}
    binaries={str(CLI):sha(CLI),str(POP):sha(POP)}
    tasks=[]
    for name,spec in scenarios.items():
        base=SOURCE/'configs/populations'/f'{name}_base.json'
        edges=SOURCE/'configs/populations'/f"{spec['graph']}_edges.csv"
        tasks.append(('populations',name,[str(POP),'--base',str(base),'--edges',str(edges),'--neurons','500'],
                      [base,edges],['events.csv','trace.csv','summary.json','base.json'],SOURCE/'results/populations/raw'))
    for name in old_rings['cases']:
        path=SOURCE/'configs/rings'/(name+'.json')
        tasks.append(('rings',name,[str(CLI),'--config',str(path)],[path],
                      ['events.csv','trace.csv','weights.csv','summary.json','experiment.json'],SOURCE/'results/rings/raw'))
    assert len(scenarios)==15 and len(old_rings['cases'])==43 and len(tasks)==58
    guarded={str(p):sha(p) for p in list((SOURCE/'configs').rglob('*')) if p.is_file()}
    for p in [SOURCE/'results/rings/metrics.json',SOURCE/'results/populations/metrics.json',SOURCE/'README.md',SOURCE/'SHORT_REPORT_RU.md']:
        guarded[str(p)]=sha(p)
    if args.skip_run:
        runs=load(HERE/'run_manifest.json')
        assert load(HERE/'binaries.json')==binaries,'Current binary differs from the replay'
        for run in runs:
            for suffix,expected in run['outputs_sha256'].items():
                assert sha(HERE/run['kind']/'raw'/(run['name']+'_'+suffix))==expected
    else:
        save(HERE/'binaries.json',binaries);runs=[]
        started=time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures=[executor.submit(replay,task) for task in tasks]
            for future in concurrent.futures.as_completed(futures):
                result=future.result();runs.append(result);save(HERE/'run_manifest.json',runs)
                print(f"{len(runs)}/58 {result['kind']}/{result['name']} {result['wall_seconds']:.2f}s; CSV identical={all(x['byte_identical'] for x in result['csv_comparisons'].values())}",flush=True)
        save(HERE/'runtime.json',dict(wall_seconds=time.perf_counter()-started,process_limit=2))
    assert {str(CLI):sha(CLI),str(POP):sha(POP)}==binaries,'Binary changed during replay'
    pop_provenance={}
    for run in runs:
        if run['kind']=='populations':
            name=run['name'];spec=scenarios[name]
            pop_provenance[name]=dict(executable_sha256=binaries[str(POP)],
                base_sha256=sha(SOURCE/'configs/populations'/f'{name}_base.json'),
                edges_sha256=sha(SOURCE/'configs/populations'/f"{spec['graph']}_edges.csv"),
                outputs=run['outputs_sha256'],wall_seconds=run['wall_seconds'],command=run['command'])
    save(HERE/'populations/run_provenance.json',pop_provenance)
    pops.analyze(scenarios,list(scenarios))
    new_pops=load(HERE/'populations/metrics.json')
    ring_metrics={};ring_equal={}
    for name,old in old_rings['cases'].items():
        config=load(SOURCE/'configs/rings'/(name+'.json'))
        value,_,_=rings.analyze(name,old['group'],config)
        ring_metrics[name]=value;ring_equal[name]=value==old
    save(HERE/'rings/metrics.json',ring_metrics)
    csv_results=[v for run in runs for v in run['csv_comparisons'].values()]
    summary=dict(saved_cases=dict(rings=43,populations=15,total=58),
        native_runs_completed=len(runs),csv_files_compared=len(csv_results),
        csv_files_byte_identical=sum(v['byte_identical'] for v in csv_results),
        all_ring_metrics_identical=all(ring_equal.values()),ring_metrics_identical=ring_equal,
        all_population_metrics_identical=new_pops==old_pops,
        population_metrics_identical={n:new_pops['runs'][n]==old_pops['runs'][n] for n in scenarios},
        population_convergence_checks=new_pops['checks'],binary_sha256=binaries,
        source_sha256={p.name:sha(p) for p in [SOURCE/'rings.py',SOURCE/'populations.py',Path(__file__)]},
        changed_saved_inputs=[p for p,h in guarded.items() if sha(p)!=h])
    assert not summary['changed_saved_inputs'],'Saved inputs changed during audit'
    save(HERE/'replay_summary.json',summary)
    print(json.dumps({k:v for k,v in summary.items() if isinstance(v,(int,bool))},ensure_ascii=False),flush=True)

if __name__=='__main__': main()
