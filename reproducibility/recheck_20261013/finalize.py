"""Wait for complete native evidence, then build the audited report sequentially."""
import argparse,subprocess,sys,time
from pathlib import Path
from replay import ROOT,read,save

def main(out):
    here=Path(__file__).resolve().parent;last=None
    while True:
        paths=list(out.glob('*/runs/*.json'));count=len(paths)
        progress=read(out/'progress.json')
        if progress['failures']:raise RuntimeError('Main replay reported failures; inspect progress.json')
        if count!=last:
            save(out/'finalization_status.json',dict(status='waiting_for_complete_native_runs',completed_records=count,required=282,epoch=time.time()))
            print('Complete-run records available:',count,'/ 282',flush=True);last=count
        if count==282:
            assert all(read(p)['complete'] for p in paths)
            ind=read(out/'independent/results.json')
            if len(ind['lab1'])==7 and len(ind['convergence'])==9:break
        time.sleep(5)
    tasks=[('manifest_scope',['--output',str(out)]),
           ('compare_optimization_archive',['--old',str(ROOT/'verification/labs_1_4_20261013'),'--new',str(out)]),
           ('integrity',['--output',str(out)]),('analyze',['--output',str(out)]),
           ('plots',['--output',str(out)]),('report',['--output',str(out)])]
    for name,args in tasks:
        save(out/'finalization_status.json',dict(status=name,epoch=time.time()));print('RUN',name,flush=True)
        p=subprocess.run([sys.executable,'-B',str(here/(name+'.py')),*args],cwd=ROOT,capture_output=True,text=True,encoding='utf-8')
        (out/(name+'_final.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
        if p.returncode:raise RuntimeError(f'{name} failed; inspect {name}_final.log')
    save(out/'finalization_status.json',dict(status='report_ready',epoch=time.time()));print('REPORT READY',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);main(ap.parse_args().output.resolve())
