"""Replay immutable laboratory inputs; keep every run separate from old evidence.

python replay.py --output verification/labs_1_4_20261013 --workers 4
Requires NumPy. The full archive includes the large, local-only CSV directories.
No changes to production sources, input configurations, or historical results.
"""
from __future__ import annotations
import argparse, concurrent.futures, csv, hashlib, itertools, json, os
from pathlib import Path
import shutil, subprocess, time, traceback
import numpy as np

ROOT = Path(__file__).resolve().parents[2]

def read(p): return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def save(p, x):
    p=Path(p);p.parent.mkdir(parents=True,exist_ok=True)
    tmp=p.with_suffix(p.suffix+'.tmp')
    tmp.write_text(json.dumps(x,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    # Windows readers may briefly deny replacement of the progress file.
    # Keep the previous file intact and retry this atomic rename only.
    deadline=time.monotonic()+2
    while True:
        try:
            tmp.replace(p)
            break
        except PermissionError:
            if time.monotonic()>=deadline:raise
            time.sleep(.025)
def sha(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()
def suff(p, s):return Path(str(p)+'_'+s)

def jobs():
    result=[]
    def add(group,config_dir,raw_dir,pattern='*.json'):
        for p in sorted((ROOT/config_dir).glob(pattern)):
            result.append(dict(group=group,name=p.stem,config=str(p),old_prefix=str(ROOT/raw_dir/p.stem),kind='cli'))
    add('lab1','lab1/configs','lab1/results/raw')
    add('lab2_rings','lab2/configs/rings','lab2/results/rings/raw')
    add('lab3','lab3/configs','lab3/results/raw')
    add('lab3_calibrated','verification/repro_labs_20261012/lab3/configs','verification/repro_labs_20261012/lab3/raw','calibrated_*.json')
    add('lab3_network','verification/repro_labs_20261012/lab3/configs','verification/repro_labs_20261012/lab3/raw','network_net*.json')
    add('lab3_network_convergence','verification/repro_labs_20261012/lab3/configs','verification/repro_labs_20261012/lab3/raw','network_benchmark*.json')
    for part in ('updated','paths','duration'):
        add('lab4_'+part,'reproducibility/lab5/'+part+'/configs','reproducibility/lab5/'+part+'/raw')
    for name,spec in read(ROOT/'lab2/results/populations/manifest.json')['scenarios'].items():
        result.append(dict(group='lab2_populations',name=name,kind='population',
            config=str(ROOT/'lab2/configs/populations'/f'{name}_base.json'),
            edges=str(ROOT/'lab2/configs/populations'/f"{spec['graph']}_edges.csv"),
            role=spec['role'],old_prefix=str(ROOT/'lab2/results/populations/raw'/name)))
    expected={'lab1':45,'lab2_rings':43,'lab3':17,'lab3_calibrated':11,'lab3_network':3,
              'lab3_network_convergence':5,'lab4_updated':36,'lab4_paths':36,'lab4_duration':30,'lab2_populations':15}
    assert {g:sum(j['group']==g for j in result) for g in expected}==expected
    for j in result:
        c=read(j['config']);assert all(s.get('delay',0)==0 for s in c['synapses'])
        j.update(config_sha256=sha(j['config']),duration=c['duration'],dt=c['dt'],neurons=500 if j['kind']=='population' else len(c['neurons']))
        j['old_summary_sha256']=sha(suff(j['old_prefix'],'summary.json'))
        if 'edges' in j:j['edges_sha256']=sha(j['edges'])
    # Start the longest full-network work early; other jobs use the fourth slot.
    result.sort(key=lambda j:(0 if j['group']=='lab3_network' else 3 if j['group']=='lab2_populations' else 2 if j['group']=='lab3_network_convergence' else 1,j['group'],j['name']))
    return result

def csv_compare(old,new):
    out=dict(old_sha256=sha(old),new_sha256=sha(new))
    out['byte_identical']=out['old_sha256']==out['new_sha256']
    with Path(old).open(encoding='utf-8-sig',newline='') as a,Path(new).open(encoding='utf-8-sig',newline='') as b:
        ra,rb=csv.reader(a),csv.reader(b);ha,hb=next(ra),next(rb)
        assert ha==hb,(old,'CSV header changed')
        maxima=np.zeros(len(ha));squares=np.zeros(len(ha));rows=0;clock_error=0.
        while True:
            ca,cb=list(itertools.islice(ra,4096)),list(itertools.islice(rb,4096))
            if not ca and not cb:break
            if len(ca)!=len(cb):return dict(**out,aligned=False,reason='row_count_changed')
            aa,bb=np.asarray(ca,dtype=float),np.asarray(cb,dtype=float)
            assert np.isfinite(bb).all(),new
            d=np.abs(aa-bb);maxima=np.maximum(maxima,d.max(axis=0));squares+=np.sum(d*d,axis=0);rows+=len(d)
            clock_error=max(clock_error,float(d[:,0].max()))
        out.update(aligned=clock_error<1e-10,rows=rows,max_clock_error_s=clock_error,
                   max_abs_by_column=dict(zip(ha,maxima.tolist())),rms_by_column=dict(zip(ha,np.sqrt(squares/max(1,rows)).tolist())))
    return out

def load_events(prefix):
    with suff(prefix,'events.csv').open(encoding='utf-8-sig',newline='') as f:
        rows=list(csv.DictReader(f))
    assert all(r['type']=='spike' for r in rows)
    t=np.asarray([float(r['time_s']) for r in rows]);n=np.asarray([int(r['node_index_1based']) for r in rows],dtype=int)
    assert np.isfinite(t).all() and (np.diff(t)>=0).all()
    return t,n

def event_compare(old,new,count):
    a,ia=load_events(old);b,ib=load_events(new)
    ca=np.bincount(ia,minlength=count+1)[1:];cb=np.bincount(ib,minlength=count+1)[1:]
    errs=[];first=[]
    for i in range(1,count+1):
        x,y=a[ia==i],b[ib==i]
        if len(x) and len(y):first.append(abs(x[0]-y[0]))
        if len(x)==len(y) and len(x):errs.extend(abs(x-y).tolist())
    return dict(old_count=len(a),new_count=len(b),old_per_node=ca.tolist(),new_per_node=cb.tolist(),
        counts_equal=bool(np.array_equal(ca,cb)),nodes_with_changed_count=int(sum(ca!=cb)),
        max_ordinal_shift_s=max(errs,default=None),median_ordinal_shift_s=float(np.median(errs)) if errs else None,
        max_first_spike_shift_s=max(first,default=None),
        byte_identical=sha(suff(old,'events.csv'))==sha(suff(new,'events.csv')))

def execute(j,out,analyze_existing=False):
    dest=out/j['group'];prefix=dest/'raw'/j['name'];dest.mkdir(parents=True,exist_ok=True);prefix.parent.mkdir(exist_ok=True)
    record=dest/'runs'/(j['name']+'.json')
    if record.exists():
        r=read(record)
        assert r['input']==j and r.get('complete'),record
        assert all(sha(Path(p))==h for p,h in r['outputs_sha256'].items()),record
        return r
    exe=out/'binaries'/('network_studio_population.exe' if j['kind']=='population' else 'network_studio_cli.exe')
    cmd=[str(exe),'--output',str(prefix)]
    if j['kind']=='population':cmd+=['--base',j['config'],'--edges',j['edges'],'--neurons','500']
    else:cmd+=['--config',j['config']]
    if analyze_existing:
        running=read(dest/'running'/(j['name']+'.json'));assert running['command']==cmd
        result=dict(input=j,command=cmd,wall_seconds=None,returncode=0,complete=False,reanalyzed_existing=True)
        assert suff(prefix,'summary.json').is_file() and 'Completed ' in suff(prefix,'cli.log').read_text('utf-8')
    else:
        started=time.time();save(dest/'running'/(j['name']+'.json'),dict(command=cmd,started_epoch=started))
        print('START',j['group'],j['name'],flush=True)
        p=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace')
        suff(prefix,'cli.log').write_text(p.stdout+p.stderr,encoding='utf-8')
        result=dict(input=j,command=cmd,wall_seconds=time.time()-started,returncode=p.returncode,complete=False)
        if p.returncode:
            result['error']=p.stdout+p.stderr;save(record,result);return result
    s=read(suff(prefix,'summary.json'));old=read(suff(j['old_prefix'],'summary.json'))
    assert s['complete'] and not s['error'] and s['time_s']==j['duration'],s
    assert sha(j['config'])==j['config_sha256']
    assert sha(suff(j['old_prefix'],'summary.json'))==j['old_summary_sha256']
    ev=event_compare(j['old_prefix'],prefix,j['neurons']);assert ev['new_per_node']==s['neuron_spikes']
    comparisons={'events':ev,'trace':csv_compare(suff(j['old_prefix'],'trace.csv'),suff(prefix,'trace.csv'))}
    if j['kind']=='cli' and suff(j['old_prefix'],'weights.csv').exists():
        comparisons['weights']=csv_compare(suff(j['old_prefix'],'weights.csv'),suff(prefix,'weights.csv'))
    tails=['events.csv','trace.csv','summary.json','cli.log']+(['base.json'] if j['kind']=='population' else ['weights.csv','deliveries.csv','pending_deliveries.csv','experiment.json'])
    result.update(complete=True,summary=s,old_summary=old,comparison=comparisons,
                  outputs_sha256={str(suff(prefix,tail)):sha(suff(prefix,tail)) for tail in tails})
    save(record,result)
    print('DONE',j['group'],j['name'],result['wall_seconds'],'seconds; spikes',ev['old_count'],'->',ev['new_count'],flush=True)
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--workers',type=int,default=4);ap.add_argument('--groups',nargs='*');ap.add_argument('--prepare',action='store_true')
    ap.add_argument('--analyze-existing',action='store_true')
    args=ap.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    plan=jobs()
    save(out/'plan.json',plan)
    if args.prepare:
        print({g:sum(j['group']==g for j in plan) for g in sorted({j['group'] for j in plan})});return
    # Run only after the standard build has completed.
    for name in ('network_studio_cli.exe','network_studio_population.exe','network_studio.exe'):
        source=ROOT/'build/Release'/name;target=out/'binaries'/name;target.parent.mkdir(exist_ok=True)
        if target.exists():assert sha(target)==sha(source),'Build changed during audit'
        else:shutil.copy2(source,target)
    save(out/('reanalysis_provenance.json' if args.analyze_existing else 'provenance.json'),dict(git_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        executables={p.name:sha(p) for p in (out/'binaries').glob('*.exe')},script_sha256=sha(__file__),
        generated_at_epoch=time.time(),workers=args.workers,zero_delay=True))
    if args.groups:plan=[j for j in plan if j['group'] in args.groups]
    results=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs={pool.submit(execute,j,out,args.analyze_existing):j for j in plan}
        for f in concurrent.futures.as_completed(futs):
            try:results.append(f.result())
            except Exception:
                error=dict(job=futs[f],error=traceback.format_exc());results.append(error);print(error,flush=True)
            save(out/('reanalysis_progress.json' if args.analyze_existing else 'progress.json'),dict(completed=len(results),total=len(plan),failures=[r for r in results if not r.get('complete')]))
    assert all(r.get('complete') for r in results),'Some replay jobs failed; see progress.json'
    print('ALL REPLAYS COMPLETE',len(results),flush=True)

if __name__=='__main__':main()
