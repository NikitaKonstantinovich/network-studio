"""Compare GUI/CLI and the preserved checkpoint build at zero delay."""
import argparse, concurrent.futures, subprocess, time
from pathlib import Path
from replay import ROOT, read, save, sha, suff

CASES=[('lab1','03_autonomous','lab1/configs'),
       ('lab2_rings','exp_n10_w0p7','lab2/configs/rings'),
       ('lab3','f5','lab3/configs'),
       ('lab4_updated','t1_pre_post_1Hz_dt25us','reproducibility/lab5/updated/configs'),
       ('lab4_updated','t2a_triplet_pre_post_10Hz_dt25us','reproducibility/lab5/updated/configs'),
       ('lab4_paths','shortcut_seed00_noise3_dt25us','reproducibility/lab5/paths/configs')]

def main(out):
    previous=ROOT/'verification/delays_20261012/baseline/network_studio_cli.exe'
    assert sha(previous)=='99638ad1a06e87cc7d1f448e09b1391fce4d585b876b1efb186086096927712b'
    gui=out/'binaries/network_studio.exe';results=[]
    def run(item):
        kind,idx=item;group,name,directory=CASES[idx];config=ROOT/directory/(name+'.json')
        target=out/'interfaces'/kind/name;target.mkdir(parents=True,exist_ok=True)
        prefix=target/'demo'
        cmd=[str(gui),str(config),'--smoke',str(target)] if kind=='gui' else [str(previous),'--config',str(config),'--output',str(prefix)]
        stamp=target/'execution.json'
        if not stamp.exists():
            start=time.time();startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
            p=subprocess.run(cmd,capture_output=True,text=True,startupinfo=startup);assert p.returncode==0,p.stderr
            save(stamp,dict(command=cmd,wall_seconds=time.time()-start,returncode=p.returncode,config_sha256=sha(config)))
        assert read(suff(prefix,'summary.json'))['complete']
        if kind=='gui':assert 'error=\n' in (target/'renderer.txt').read_text()
        return dict(kind=kind,group=group,name=name,prefix=str(prefix),command=cmd)
    tasks=[('checkpoint_build',i) for i in range(len(CASES))]+[('gui',i) for i in range(4)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for r in pool.map(run,tasks):results.append(r);save(out/'interfaces/executions.json',results);print(r['kind'],r['name'],'complete',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    main(ap.parse_args().output.resolve())
