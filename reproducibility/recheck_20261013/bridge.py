"""Check the 500-neuron laboratory population against the GUI's full core."""
import argparse
from pathlib import Path
import numpy as np
from replay import ROOT, read, save, sha, suff, execute, load_events

def main(out):
    old=ROOT/'verification/repro_labs_20261012/lab2/bridge_500';rows=[]
    for kind,name,config in [('cli','core','config.json'),('population','population','base.json')]:
        cp=old/config;c=read(cp)
        j=dict(group='lab2_bridge',name=name,kind=kind,config=str(cp),old_prefix=str(old/name),
            config_sha256=sha(cp),duration=c['duration'],dt=c['dt'],neurons=500,old_summary_sha256=sha(suff(old/name,'summary.json')))
        if kind=='population':
            j['edges']=str(ROOT/'lab2/configs/populations/k30_seed42_edges.csv');j['edges_sha256']=sha(j['edges'])
        rows.append(execute(j,out))
    a,ia=load_events(out/'lab2_bridge/raw/core');b,ib=load_events(out/'lab2_bridge/raw/population')
    equal=rows[0]['summary']['neuron_spikes']==rows[1]['summary']['neuron_spikes'];assert equal
    error=max(float(max(abs(a[ia==i]-b[ib==i]),default=0)) for i in range(1,501))
    x=np.genfromtxt(out/'lab2_bridge/raw/core_trace.csv',delimiter=',',names=True)
    y=np.genfromtxt(out/'lab2_bridge/raw/population_trace.csv',delimiter=',',names=True)
    voltage=max(float(max(abs(x[f'N{i}_V_mV']-y[f'N{i}_v_mV']))) for i in range(1,5))
    result=dict(neurons=500,duration_s=.2,spikes=len(a),per_node_counts_equal=equal,max_event_difference_s=error,max_voltage_difference_mV=voltage,
        explanation='The aggregated state has a different error-norm dimension; check numerical agreement, not bit equality.')
    save(out/'bridge_comparison.json',result);print(result,flush=True)
    assert error<1e-7 and voltage<1e-3,result

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    main(ap.parse_args().output.resolve())
