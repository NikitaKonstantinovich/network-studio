"""Verify protocol migrations, raw-file hashes, and fixed-seed repeats."""
import argparse, copy
from pathlib import Path
from replay import ROOT, read, save, sha, suff

def canonical(c):
    c=copy.deepcopy(c);schema=c['schema'];assert schema in (2,3,4,5)
    if schema==2:
        for s in c['synapses']:
            s.update(stdp=False,stdp_rule='additive',a_plus=.01,a_minus=.012,tau_plus=.02,tau_minus=.02,weight_min=0.,weight_max=2.)
            if s['model']=='conductance_stp':s['weight']=1.
    if schema<=3:
        c.update(record_all=True,record_neurons=[],record_synapses=[])
        for n in c['neurons']:n['inhibitory']=False
    for n in c['neurons']:n.setdefault('pulse_intervals',[])
    for s in c['synapses']:
        s.setdefault('tau_slow',.1);s.setdefault('delay',0.);s.setdefault('stdp_timing','arrival')
    c['schema']=5
    return c

def differences(a,b,path=''):
    if type(a)==dict and type(b)==dict:
        return [d for k in sorted(a.keys()|b.keys()) for d in
            (differences(a[k],b[k],path+'.'+k) if k in a and k in b else [dict(path=path+'.'+k,old=a.get(k),new=b.get(k))])]
    if type(a)==list and type(b)==list and len(a)==len(b):
        return [d for i,(x,y) in enumerate(zip(a,b)) for d in differences(x,y,path+f'[{i}]')]
    return [] if a==b else [dict(path=path,old=a,new=b)]

def main(out):
    rows=[]
    for p in sorted(out.glob('*/runs/*.json')):
        r=read(p);assert r['complete'];j=r['input'];c=read(j['config']);prefix=out/j['group']/'raw'/j['name']
        snapshot='base.json' if j['kind']=='population' else 'experiment.json'
        before=read(suff(j['old_prefix'],snapshot));after=read(suff(prefix,snapshot))
        di=differences(canonical(before),canonical(after))
        dc=differences(canonical(c),canonical(after))
        assert sha(j['config'])==j['config_sha256']
        assert sha(suff(j['old_prefix'],'summary.json'))==j['old_summary_sha256']
        assert all(sha(p)==h for p,h in r['outputs_sha256'].items())
        rows.append(dict(group=j['group'],name=j['name'],archive_to_new=di,input_to_new=dc))
    repeats={}
    for group,left,right in [('lab1','07_noise_individual','repeat_noise'),('lab2_populations','k30_seed42_short','k30_seed42_repeat')]:
        if not suff(out/group/'raw'/right,'summary.json').exists():continue
        repeats[group]={s:sha(suff(out/group/'raw'/left,s))==sha(suff(out/group/'raw'/right,s)) for s in ('trace.csv','events.csv')}
        assert all(repeats[group].values()),repeats
    result=dict(checked=len(rows),configurations_identical_after_explicit_migration=all(not r['archive_to_new'] and not r['input_to_new'] for r in rows),
        differences=[r for r in rows if r['archive_to_new'] or r['input_to_new']],repeats=repeats)
    save(out/'integrity.json',result);print(result,flush=True)
    assert result['configurations_identical_after_explicit_migration']

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    main(ap.parse_args().output.resolve())
