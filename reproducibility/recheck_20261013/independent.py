"""Fresh event-located DOP853 checks and third-step laboratory controls."""
import argparse, copy, csv, subprocess, time
from pathlib import Path
import numpy as np
from replay import ROOT, read, save, sha, suff, load_events
from analyze import module, replay_weights

def run(c,name,out):
    cfg=out/'independent/configs'/(name+'.json');prefix=out/'independent/raw'/name
    save(cfg,c);prefix.parent.mkdir(parents=True,exist_ok=True)
    exe=out/'binaries/network_studio_cli.exe';stamp=suff(prefix,'run.json')
    if stamp.exists():
        r=read(stamp);assert r['config_sha256']==sha(cfg) and r['exe_sha256']==sha(exe)
    else:
        start=time.time();cmd=[str(exe),'--config',str(cfg),'--output',str(prefix)]
        p=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8');assert not p.returncode,p.stderr
        suff(prefix,'cli.log').write_text(p.stdout+p.stderr,encoding='utf-8')
        s=read(suff(prefix,'summary.json'));assert s['complete'] and not s['error']
        save(stamp,dict(command=cmd,config_sha256=sha(cfg),exe_sha256=sha(exe),wall_seconds=time.time()-start))
    return prefix

def compare(reference,prefix):
    t,n=load_events(prefix);native=t[n==1];expected=np.asarray(reference['events_s'])
    count=len(native)==len(expected)
    return dict(spikes=len(native),reference_spikes=len(expected),same_spike_count=count,
        max_event_error_ms=float(max(abs(native-expected),default=0)*1000) if count else None)

def main(out):
    refmod=module('fresh_lab1_reference',ROOT/'reproducibility/audit_sources/lab1/audit.py')
    names=read(ROOT/'lab1/results/manifest.json')['deterministic']
    result=dict(lab1={},convergence={},reference_source_sha256=sha(ROOT/'reproducibility/audit_sources/lab1/audit.py'))
    for name in names:
        c=read(ROOT/'lab1/configs'/(name+'.json'));ref=refmod.adaptive(c)
        save(out/'independent/references'/(name+'.json'),ref)
        x=dict(archive_25us=compare(ref,ROOT/'lab1/results/raw'/name),
               current_25us=compare(ref,out/'lab1/raw'/name),current_12p5us=compare(ref,out/'lab1/raw'/('fine_'+name)))
        fine=copy.deepcopy(c);fine['dt']/=4
        x['current_6p25us']=compare(ref,run(fine,'lab1_'+name+'_6p25us',out))
        assert all(v['same_spike_count'] for v in x.values()),(name,x)
        result['lab1'][name]=x;save(out/'independent/results.json',result);print(name,x,flush=True)
    # Identical original laboratory protocols, only dt changed to a third value.
    fixtures=[('lab2_rings','tm_n03_w1','lab2/configs/rings'),
        ('lab2_rings','tm_n10_w1','lab2/configs/rings'),
        ('lab2_rings','exp_n10_w0p7','lab2/configs/rings'),
        ('lab3','f5','lab3/configs'),('lab3','f100','lab3/configs'),
        ('lab4_updated','t1_pre_post_10Hz_dt25us','reproducibility/lab5/updated/configs'),
        ('lab4_updated','t1_post_pre_10Hz_dt25us','reproducibility/lab5/updated/configs'),
        ('lab4_updated','t2a_triplet_pre_post_10Hz_dt25us','reproducibility/lab5/updated/configs'),
        ('lab4_updated','t2b_triplet_pre_post_pre_1Hz_dt25us','reproducibility/lab5/updated/configs')]
    for group,name,directory in fixtures:
        c=read(ROOT/directory/(name+'.json'));c['dt']/=4
        prefix=run(c,group+'_'+name+'_6p25us',out)
        s=read(suff(prefix,'summary.json'));item=dict(dt_us=c['dt']*1e6,spikes=s['neuron_spikes'],weights=s['final_synapse_weights'],prefix=str(prefix))
        if group=='lab4_updated':item['replay_errors']=replay_weights(prefix,c)
        result['convergence'][group+'/'+name]=item
        save(out/'independent/results.json',result);print('THIRD STEP',group,name,item,flush=True)
    print('INDEPENDENT CHECKS COMPLETE',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True)
    main(ap.parse_args().output.resolve())
