"""Bitwise comparison of original/optimized executables; no tolerance or checkpoint migration."""
import argparse, concurrent.futures, copy, hashlib, struct, subprocess, time
from pathlib import Path
from replay import ROOT, read, save, sha, suff

def payload(path):
    b=Path(path).read_bytes();magic=b'NETWORK-STUDIO-CHECKPOINT\n';assert b.startswith(magic)
    pos=len(magic)+8
    n=struct.unpack_from('<Q',b,pos)[0];pos+=8+n # algorithm remains compared
    start=pos;n=struct.unpack_from('<Q',b,pos)[0];pos+=8+n
    # Only build identity and the dependent checksum differ. All config, state,
    # adaptive state, traces, flags, queues, counts and history remain byte-exact.
    return b[:start]+b[pos:-8]

def main(old,new,out,reference=None,only=None):
    out.mkdir(parents=True,exist_ok=True);plan=[]
    previous=read(reference/'results.json') if reference else None
    if previous:assert previous['original_binaries']=={p.name:sha(p) for p in old.glob('*.exe')}
    for i in range(8):plan.append((f'preset_{i}',['--preset',str(i),'--duration','0.15'],False))
    for p in sorted((ROOT/'examples').glob('*.json')):
        if p.stem=='empty':continue
        plan.append((p.stem,['--config',str(p),'--duration','0.15'],False))
    c=read(ROOT/'examples/four_neurons.json');c['duration']=.07
    for i,s in enumerate(c['synapses']):
        s['tau_f']=.04+i*.013;s['tau_d']=.08+i*.017;s['tau_syn']=.003+i*.0007
    cp=out/'mixed_kinetics.json';save(cp,c);plan.append(('mixed_kinetics',['--config',str(cp)],False))
    for tag,p in [('tm',ROOT/'lab2/configs/populations/k15_seed42_base.json'),
                  ('large_core',ROOT/'verification/repro_labs_20261012/lab3/configs/network_benchmark.json')]:
        c=read(p);c['duration']=.03;c['sample']=.0005;cp=out/(tag+'.json');save(cp,c)
        args=['--base',str(cp),'--edges',str(ROOT/'lab2/configs/populations/k15_seed42_edges.csv'),'--neurons','500'] if tag=='tm' else ['--config',str(cp)]
        plan.append((tag,args,tag=='tm'))
    for model in ('tsodyks_markram','exponential_current'):
        c=read(ROOT/'lab2/configs/populations/k15_seed42_base.json');c['duration']=.2;c['sample']=.001
        c['neurons'][0]['current']=4;c['synapses'][0]['model']=model
        if model=='exponential_current':c['synapses'][0]['gain']=1
        cp=out/(model+'_active.json');save(cp,c)
        plan.append((model+'_active',['--base',str(cp),'--edges',str(ROOT/'lab2/configs/populations/k15_seed42_edges.csv'),
            '--neurons','500','--record',','.join(str(i*31) for i in range(16))],True))
    def run(j):
        name,args,pop=j;times={};prefixes=[]
        for label,bindir in [('original',old),('optimized',new)]:
            if label=='original' and previous:
                prefixes.append(reference/'original'/name)
                times[label]=next(r['wall_seconds'][label] for r in previous['cases'] if r['name']==name)
                continue
            prefix=out/label/name;prefix.parent.mkdir(parents=True,exist_ok=True);prefixes.append(prefix)
            exe=bindir/('network_studio_population.exe' if pop else 'network_studio_cli.exe')
            cmd=[str(exe),*args,'--output',str(prefix)]
            if not pop:cmd+=['--checkpoint',str(prefix)+'.nscp']
            start=time.perf_counter();p=subprocess.run(cmd,capture_output=True,text=True);times[label]=time.perf_counter()-start
            suff(prefix,'cli.log').write_text(p.stdout+p.stderr,encoding='utf-8');assert p.returncode==0,(cmd,p.stderr)
        a,b=prefixes;files={}
        for suffix in (('events.csv','trace.csv') if pop else ('events.csv','trace.csv','weights.csv','deliveries.csv','pending_deliveries.csv')):
            p=suff(a,suffix);q=suff(b,suffix);assert p.read_bytes()==q.read_bytes(),(name,suffix)
            files[suffix]=sha(p)
        sa,sb=read(suff(a,'summary.json')),read(suff(b,'summary.json'))
        if name.endswith('_active'):assert sum(sa['neuron_spikes'])>0,'Active projection fixture must contain spikes'
        for s in (sa,sb):
            s.pop('wall_seconds',None)
            if previous:s.pop('base_path',None)
        assert sa==sb,(name,'summary')
        tail='base.json' if pop else 'experiment.json'
        assert suff(a,tail).read_bytes()==suff(b,tail).read_bytes(),(name,'full exported configuration')
        if not pop:assert payload(str(a)+'.nscp')==payload(str(b)+'.nscp'),(name,'full binary checkpoint state')
        result=dict(name=name,exact=True,files=files,full_checkpoint=not pop,wall_seconds=times,speedup=times['original']/times['optimized'])
        save(out/'results'/(name+'.json'),result);print(name,'EXACT',times,flush=True);return result
    if only:plan=[j for j in plan if j[0] in only]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:results=list(pool.map(run,plan))
    save(out/'results.json',dict(cases=results,original_binaries={p.name:sha(p) for p in old.glob('*.exe')},optimized_binaries={p.name:sha(p) for p in new.glob('*.exe')}))
    print('ALL BITWISE CHECKS PASSED',len(results),flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--old',required=True,type=Path);ap.add_argument('--new',required=True,type=Path);ap.add_argument('--output',required=True,type=Path);ap.add_argument('--reference-output',type=Path);ap.add_argument('--only',nargs='+')
    a=ap.parse_args();main(a.old.resolve(),a.new.resolve(),a.output.resolve(),a.reference_output.resolve() if a.reference_output else None,a.only)
