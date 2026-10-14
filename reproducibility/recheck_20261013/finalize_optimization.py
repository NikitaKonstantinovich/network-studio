"""Revalidate and collect 24 main controls plus the bounded exponential control."""
import argparse
from pathlib import Path
from replay import ROOT,read,save,sha,suff
from optimization import payload

def main(out):
    records=list((out/'optimization_verified/results').glob('*.json'))
    assert len(records)==24
    records.append(out/'optimization_exponential_control/results/exponential_current_active.json')
    rows=[]
    for p in records:
        r=read(p);base=p.parent.parent;a=base/'original'/r['name'];b=base/'optimized'/r['name']
        assert r['exact']
        for tail,h in r['files'].items():assert sha(suff(a,tail))==h==sha(suff(b,tail)),(p,tail)
        tail='experiment.json' if r['full_checkpoint'] else 'base.json'
        assert suff(a,tail).read_bytes()==suff(b,tail).read_bytes(),(p,'config')
        if r['full_checkpoint']:assert payload(str(a)+'.nscp')==payload(str(b)+'.nscp'),p
        r.update(original_prefix=str(a),optimized_prefix=str(b),config_sha256=sha(suff(a,tail)))
        rows.append(r)
    assert len({r['name'] for r in rows})==25
    result=dict(cases=rows,original_binaries={p.name:sha(p) for p in (ROOT/'verification/labs_1_4_20261013/binaries').glob('*.exe')},
        optimized_binaries={p.name:sha(p) for p in (out/'binaries').glob('*.exe')},
        excluded_probe=dict(directory=str(out/'optimization_verified/original/exponential_current_active'),
            reason='Non-laboratory exponential stress probe with the TM gain=20 stopped during an event avalanche; no numerical outcome asserted. The completed exponential regression has gain=1; laboratory inputs were not edited.'))
    save(out/'optimization_comparison.json',result);print('Revalidated 25 bitwise controls, including 22 full binary checkpoints.')

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);main(ap.parse_args().output.resolve())
