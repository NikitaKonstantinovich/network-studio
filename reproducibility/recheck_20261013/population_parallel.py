"""Use four otherwise idle CPU slots for the long population batch.

Publish a fully verified group atomically only if the main worker has not
started it. Keep the separate raw originals: recorded provenance stays valid.
"""
import argparse, concurrent.futures, os, shutil
from pathlib import Path
from replay import jobs, execute, save, sha

def main(out):
    separate=out/'parallel_population';separate.mkdir(exist_ok=True)
    (separate/'binaries').mkdir(exist_ok=True)
    for p in (out/'binaries').glob('*.exe'):
        q=separate/'binaries'/p.name
        if q.exists():assert sha(q)==sha(p)
        else:shutil.copy2(p,q)
    plan=[j for j in jobs() if j['group']=='lab2_populations'];save(separate/'plan.json',plan)
    results=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futs=[pool.submit(execute,j,separate) for j in plan]
        for f in concurrent.futures.as_completed(futs):
            results.append(f.result());save(separate/'progress.json',dict(completed=len(results),total=len(plan)))
    assert all(r['complete'] for r in results)
    stage=out/'population_publish_staging';target=out/'lab2_populations'
    # All resolved move paths are confined to this new audit directory.
    assert stage.resolve().is_relative_to(out.resolve()) and target.resolve().is_relative_to(out.resolve())
    published=False
    if not target.exists():
        shutil.copytree(separate/'lab2_populations',stage)
        try:os.rename(stage,target)
        except FileExistsError:print('Main queue already started; retain separate verified batch.',flush=True)
        else:
            published=True
            print('Published verified population batch; main queue will reuse it.',flush=True)
    save(separate/'complete.json',dict(complete=True,runs=len(results),published=published,extra_workers=4))

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    main(ap.parse_args().output.resolve())
