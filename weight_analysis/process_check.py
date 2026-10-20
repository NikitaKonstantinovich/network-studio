"""Check the ordinary checkpoint loader through the weights CLI in new processes."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--checkpoint', required=True, type=Path)
p.add_argument('--incompatible', required=True, type=Path)
p.add_argument('--output', type=Path)
args = p.parse_args()
out = args.output or ROOT/'verification/weight_snapshots_20261020'/('process_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
out.mkdir(parents=True,exist_ok=False)
cli = ROOT/'build/Release/network_studio_cli.exe'
def digest(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()
before = {str(p):digest(p) for p in (args.checkpoint,args.incompatible)}
start = time.perf_counter()
accepted = subprocess.run([str(cli),'--weights-from-checkpoint',str(args.checkpoint),'--output',str(out/'accepted.nsweights')],capture_output=True)
rejected = subprocess.run([str(cli),'--weights-from-checkpoint',str(args.incompatible),'--output',str(out/'rejected.nsweights')],capture_output=True)
(out/'accepted.log').write_bytes(accepted.stdout+accepted.stderr)
(out/'rejected.log').write_bytes(rejected.stdout+rejected.stderr)
assert accepted.returncode == 0 and (out/'accepted.nsweights').exists()
assert rejected.returncode != 0 and not (out/'rejected.nsweights').exists()
assert b'unsupported schema' in rejected.stderr or b'incompatible' in rejected.stderr, rejected.stderr
assert before == {str(p):digest(p) for p in (args.checkpoint,args.incompatible)}
result = {'wall_seconds':time.perf_counter()-start,'cli_sha256':digest(cli),'inputs_unchanged':True,'input_sha256':before,'accepted_exit':accepted.returncode,'rejected_exit':rejected.returncode,'rejection':rejected.stderr.decode('utf-8'),'simulation_steps_executed':0}
(out/'verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(out)
print(json.dumps(result,ensure_ascii=False))
