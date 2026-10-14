"""Compare every completed original-build replay with its optimized-build replay."""
import argparse
from pathlib import Path
from replay import read,save,sha,suff

def main(old,new):
    results=[]
    for p in sorted(old.glob('*/runs/*.json')):
        r=read(p)
        if not r.get('complete'):continue
        j=r['input'];q=new/j['group']/'runs'/p.name
        s=read(q);assert s['complete'] and s['input']==j
        a=old/j['group']/'raw'/j['name'];b=new/j['group']/'raw'/j['name'];files={}
        for tail in (('events.csv','trace.csv') if j['kind']=='population' else ('events.csv','trace.csv','weights.csv','deliveries.csv','pending_deliveries.csv')):
            f=suff(a,tail);files[tail]=sha(f)==sha(suff(b,tail))
        assert all(files.values()),(p,files)
        x,y=r['summary'],s['summary']
        for v in (x,y):v.pop('wall_seconds',None)
        assert x==y,(p,'summary')
        results.append(dict(group=j['group'],name=j['name'],byte_identical=files))
    save(new/'optimization_archive_comparison.json',dict(cases=len(results),csv_files=sum(len(r['byte_identical']) for r in results),results=results))
    print('ALL ORIGINAL-BUILD FINISHED REPLAYS MATCH EXACTLY',len(results))

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--old',type=Path,required=True);ap.add_argument('--new',type=Path,required=True)
    a=ap.parse_args();main(a.old.resolve(),a.new.resolve())
