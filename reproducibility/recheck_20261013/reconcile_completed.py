"""Verify completed native outputs and recover a progress counter after Windows sharing contention."""
import argparse
from pathlib import Path
from replay import read,save,execute,sha,suff

def main(out):
    plan=read(out/'plan.json')
    assert len(plan)==241
    # Never start a calculation here: require its completed record first.
    for j in plan:
        r=read(out/j['group']/'runs'/(j['name']+'.json'))
        assert r['complete'] and r['returncode']==0 and r['input']==j
        assert sha(j['config'])==j['config_sha256']
        assert sha(suff(j['old_prefix'],'summary.json'))==j['old_summary_sha256']
    records=[execute(j,out) for j in plan] # Existing-record branch verifies every output SHA-256.
    assert len(records)==241 and all(r['complete'] for r in records)
    prior=out/'progress_before_reconcile.json'
    if not prior.exists():save(prior,read(out/'progress.json'))
    save(out/'progress.json',dict(completed=len(records),total=len(plan),failures=[]))
    save(out/'progress_reconciliation.json',dict(reason='The completed main dispatcher reported WinError 5 while atomically replacing progress.json. Native worker results completed independently.',
        verified_records=len(records),native_returncodes=sorted(set(r['returncode'] for r in records)),
        simulation_reruns=0,original_log_sha256=sha(out/'replay.log')))
    print('VERIFIED COMPLETED RUNS AND RECOVERED PROGRESS:',len(records),'native reruns: 0',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path)
    main(ap.parse_args().output.resolve())
