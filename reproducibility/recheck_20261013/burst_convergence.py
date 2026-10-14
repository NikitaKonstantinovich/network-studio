"""Resolve the complete first sparse-input network burst after a large archive change."""
import argparse, concurrent.futures, copy, subprocess, time
from pathlib import Path
import numpy as np
from replay import ROOT, read, save, sha, suff, load_events

def main(out):
    dest=out/'burst_convergence';dest.mkdir(exist_ok=True)
    archived=read(out/'lab3_network/runs/network_net1_0p1Hz.json')
    base=read(archived['input']['config']);exe=out/'binaries/network_studio_cli.exe'
    assert sha(exe)==read(out/'provenance.json')['executables'][exe.name]
    def execute(dt):
        name=f'burst_{dt*1e6:g}us';prefix=dest/name;cfg=dest/(name+'.json')
        c=copy.deepcopy(base);c.update(dt=dt,duration=.5,name=base['name']+' / complete first burst refinement')
        save(cfg,c);command=[str(exe),'--config',str(cfg),'--output',str(prefix)]
        print('START',name,flush=True);started=time.time()
        p=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,encoding='utf-8')
        suff(prefix,'cli.log').write_text(p.stdout+p.stderr,encoding='utf-8')
        assert p.returncode==0,(name,p.stdout,p.stderr)
        s=read(suff(prefix,'summary.json'));assert s['complete'] and not s['error'] and s['time_s']==.5
        print('COMPLETE',name,sum(s['neuron_spikes']),flush=True)
        return dict(dt_us=dt*1e6,prefix=str(prefix),command=command,wall_seconds=time.time()-started,
                    config_sha256=sha(cfg),outputs_sha256={str(f):sha(f) for f in dest.glob(name+'_*') if f.is_file()})
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        runs=list(pool.map(execute,[12.5e-6,6.25e-6]))
    rows=[dict(dt_us=25.,prefix=str(out/'lab3_network/raw/network_net1_0p1Hz'),source='completed 30-second replay, restricted to [0, 0.5)')]+runs
    reference=rows[-1]
    def data(r):
        t,n=load_events(r['prefix']);m=t<.5
        trace=np.genfromtxt(suff(r['prefix'],'trace.csv'),delimiter=',',names=True)
        return t[m],n[m],trace[trace['time_s']<.5-1e-12]
    rt,rn,rv=data(reference)
    for r in rows:
        t,n,v=data(r);same=True;error=0.
        for node in range(1,501):
            a,b=t[n==node],rt[rn==node]
            if len(a)!=len(b):same=False
            elif len(a):error=max(error,float(np.max(abs(a-b))))
        assert np.array_equal(v['time_s'],rv['time_s'])
        columns=[c for c in v.dtype.names if c.endswith('_V_mV')]
        delta=np.concatenate([v[c]-rv[c] for c in columns])
        r.update(spikes=len(t),active_neurons=len(set(n)),last_spike_s=float(t[-1]),
                 same_per_neuron_counts_as_finest=same,max_event_difference_ms=1000*error if same else None,
                 recorded_voltage_rms_difference_mV=float(np.sqrt(np.mean(delta*delta))))
    result=dict(duration_s=.5,reason='Archive 0.1 Hz changes 4864 to 7438 spikes; earlier 0.2 s refinement missed burst tail.',
                executable_sha256=sha(exe),reference_dt_us=6.25,rows=rows)
    save(dest/'results.json',result);print('BURST CONVERGENCE COMPLETE',[(r['dt_us'],r['spikes']) for r in rows],flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True)
    main(ap.parse_args().output.resolve())
