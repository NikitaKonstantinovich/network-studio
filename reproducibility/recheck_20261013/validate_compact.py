"""Validate the independent-linear-suffix optimization against frozen full states."""
import argparse,concurrent.futures,subprocess,time
from pathlib import Path
from replay import read,save,sha,suff,ROOT
from optimization import payload

def main(reference,out):
    source=read(reference/'optimization_comparison.json');bindir=out/'binaries'
    def run(r):
        old=Path(r['original_prefix']);name=r['name'];pop=not r['full_checkpoint']
        prefix=out/'optimization_verified/optimized'/name;prefix.parent.mkdir(parents=True,exist_ok=True)
        summary=read(suff(old,'summary.json'))
        config=suff(old,'base.json' if pop else 'experiment.json')
        exe=bindir/('network_studio_population.exe' if pop else 'network_studio_cli.exe')
        args=['--base',str(config),'--edges',summary['edges_path'],'--neurons','500',
              '--record',','.join(map(str,summary['recorded_neurons_0based']))] if pop else ['--config',str(config),'--checkpoint',str(prefix)+'.nscp']
        start=time.perf_counter();p=subprocess.run([str(exe),*args,'--output',str(prefix)],capture_output=True,text=True)
        elapsed=time.perf_counter()-start;suff(prefix,'cli.log').write_text(p.stdout+p.stderr,encoding='utf-8')
        assert p.returncode==0,(name,p.stderr)
        files={}
        for tail in (('events.csv','trace.csv') if pop else ('events.csv','trace.csv','weights.csv','deliveries.csv','pending_deliveries.csv')):
            assert suff(old,tail).read_bytes()==suff(prefix,tail).read_bytes(),(name,tail)
            files[tail]=sha(suff(prefix,tail))
        a,b=summary,read(suff(prefix,'summary.json'))
        for v in (a,b):v.pop('wall_seconds',None);v.pop('base_path',None)
        assert a==b,(name,'summary')
        assert config.read_bytes()==suff(prefix,'base.json' if pop else 'experiment.json').read_bytes(),(name,'configuration')
        if not pop:assert payload(str(old)+'.nscp')==payload(str(prefix)+'.nscp'),(name,'full checkpoint')
        result=dict(r,files=files,optimized_prefix=str(prefix),exact=True,
            wall_seconds=dict(original=r['wall_seconds']['original'],optimized=elapsed),speedup=r['wall_seconds']['original']/elapsed)
        save(out/'optimization_verified/results'/(name+'.json'),result)
        print('EXACT',name,elapsed,flush=True);return result
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:results=list(pool.map(run,source['cases']))
    save(out/'optimization_comparison.json',dict(cases=results,original_binaries=source['original_binaries'],
        optimized_binaries={p.name:sha(p) for p in bindir.glob('network_studio*.exe')}))
    print('ALL 25 BITWISE CONTROLS PASSED',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--reference',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    a=ap.parse_args();main(a.reference.resolve(),a.output.resolve())
