"""Replay the saved user-report controls: noise 4/4.5, ring 11, imported graphs."""
import argparse, concurrent.futures
from pathlib import Path
from replay import ROOT, read, save, sha, suff, execute

def main(out):
    archive=ROOT/'verification/repro_labs_20261012';plan=[]
    for p in sorted((archive/'lab1/extra').glob('*.json')):
        if p.stem.endswith(('_experiment','_summary')):continue
        plan.append(dict(group='lab1_extra',name=p.stem,config=str(p),old_prefix=str(p.with_suffix('')),kind='cli'))
    for name,cp in [('tm_n11_w1','config'),('tm_n11_w1_fine','config_fine')]:
        plan.append(dict(group='lab2_ring11',name=name,config=str(archive/'lab2/ring11'/(cp+'.json')),
            old_prefix=str(archive/'lab2/ring11'/('fine' if 'fine' in cp else 'tm_n11_w1')),kind='cli'))
    for name in ('nn_k15','nn_k30'):
        base=archive/'lab2/imported_graphs'
        plan.append(dict(group='lab2_imported',name=name,kind='population',config=str(base/'configs/populations'/(name+'_base.json')),
            edges=str(base/'configs/populations'/(name+'_edges.csv')),old_prefix=str(base/'results/populations/raw'/name)))
    for j in plan:
        c=read(j['config']);j.update(config_sha256=sha(j['config']),duration=c['duration'],dt=c['dt'],
            neurons=500 if j['kind']=='population' else len(c['neurons']),old_summary_sha256=sha(suff(j['old_prefix'],'summary.json')))
        if 'edges' in j:j['edges_sha256']=sha(j['edges'])
    save(out/'extra_plan.json',plan)
    results=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for r in pool.map(lambda j:execute(j,out),plan):
            results.append(r);save(out/'extra_progress.json',dict(completed=len(results),total=len(plan)))
    assert all(r['complete'] for r in results)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    main(ap.parse_args().output.resolve())
