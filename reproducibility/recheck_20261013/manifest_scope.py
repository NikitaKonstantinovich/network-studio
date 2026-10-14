"""Repair only manifest scope from early harness versions, never data or hashes.

A prefix glob could include a concurrently running longer-named scenario.
Keep exactly the files of this run; require all retained hashes still to match.
"""
import argparse
from pathlib import Path
from replay import read,save,sha,suff

def main(out):
    changed=[]
    for p in out.glob('*/runs/*.json'):
        r=read(p)
        if not r.get('complete'):continue
        j=r['input'];prefix=Path(r['command'][r['command'].index('--output')+1])
        tails=['events.csv','trace.csv','summary.json','cli.log']+(['base.json'] if j['kind']=='population' else ['weights.csv','deliveries.csv','pending_deliveries.csv','experiment.json'])
        allowed={str(suff(prefix,tail)) for tail in tails};before=r['outputs_sha256']
        assert allowed<=before.keys(),p
        retained={k:v for k,v in before.items() if k in allowed}
        assert all(sha(k)==v for k,v in retained.items()),('Owned output changed',p)
        removed={k:v for k,v in before.items() if k not in allowed}
        if removed:
            changed.append(dict(record=str(p),excluded_foreign_files=removed))
            r['outputs_sha256']=retained;r['manifest_scope_fixed']=True;save(p,r)
    save(out/'manifest_scope_correction.json',changed);print('Corrected manifest scope only:',len(changed))

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);main(ap.parse_args().output.resolve())
