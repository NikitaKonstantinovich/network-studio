"""Duration sensitivity from recovered NeuroNet snapshot times, not a state replay.

The source snapshots differ by 6.100 s (two neurons) and 12.525 s (shortcut).
Fresh native states/noise sequences remain different from NeuroNet. The chain
control uses the same 12.525 s to isolate topology, not an inferred archive time.
"""
import argparse
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import statistics

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('path_audit',HERE/'paths/run.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)

def run(cli):
    out=HERE/'duration';(out/'raw').mkdir(parents=True,exist_ok=True)
    cli=Path(cli).resolve();cli_sha=base.sha(cli)
    rows=[]
    for topology,duration in [('two',6.1),('chain',12.525),('shortcut',12.525)]:
        for seed in range(10):
            c=base.config(topology,seed,3.,25e-6)
            c.update(duration=duration,sample=.005)
            for n in c['neurons']:n['stop']=duration
            name=f'{topology}_seed{seed:02d}_duration{duration:g}'
            c['name']='Lab5 duration sensitivity '+name
            cp=out/'configs'/(name+'.json');prefix=out/'raw'/name
            base.save(cp,c)
            r=subprocess.run([str(cli),'--config',str(cp),'--output',str(prefix)],capture_output=True,text=True,check=True)
            prefix.with_suffix('.log').write_text(r.stdout+r.stderr,encoding='utf-8')
            s=base.read(Path(str(prefix)+'_summary.json'))
            assert s['complete'] and s['time_s']==duration and not s['error']
            with Path(str(prefix)+'_events.csv').open(newline='',encoding='utf-8') as f:
                events=[(float(r['time_s']),int(r['node_index_1based'])-1) for r in csv.DictReader(f)]
            replay=base.stdp_replay(c,events)
            assert max(abs(a-b) for a,b in zip(replay,s['final_synapse_weights']))<1e-10
            rows.append(dict(case=name,topology=topology,seed=seed,duration=duration,
                spikes=s['neuron_spikes'],weights=s['final_synapse_weights'],
                replay_max_error=max(abs(a-b) for a,b in zip(replay,s['final_synapse_weights'])),
                config_sha256=base.sha(cp),raw_sha256={p.name:base.sha(p) for p in prefix.parent.glob(name+'_*')}))
            print(name,s['neuron_spikes'],s['final_synapse_weights'],flush=True)
    aggregate={}
    for topology in ('two','chain','shortcut'):
        selected=[r for r in rows if r['topology']==topology]
        aggregate[topology]=dict(duration=selected[0]['duration'],
            weights=[base.describe([r['weights'][i] for r in selected]) for i in range(len(selected[0]['weights']))])
    result=dict(scope=__doc__,cli=str(cli),cli_sha256=cli_sha,run_count=len(rows),aggregate=aggregate,rows=rows)
    base.save(out/'findings.json',result)
    assert base.sha(cli)==cli_sha

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli',required=True)
    run(parser.parse_args().cli)
