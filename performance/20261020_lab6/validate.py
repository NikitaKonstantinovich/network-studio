"""Read-only equivalence checks plus native CLI process restart. No checkpoint conversion."""
import argparse,hashlib,json,subprocess,time,struct
from pathlib import Path
import compare_checkpoint as compare
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2];data=Path(__file__).resolve().parent
start=time.perf_counter();checks=[]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def payload(p):
 b=p.read_bytes();pos=len(compare.MAGIC)+8
 for _ in range(2):n=struct.unpack_from('<Q',b,pos)[0];pos+=8+n
 return hashlib.sha256(b[pos:-8]).hexdigest()
def equal(a,b,what):
 ok=digest(a)==digest(b);checks.append(dict(check=what,equal=ok))
 if not ok:raise RuntimeError(f'{what}: {a} != {b}')
for window in ['quiet','burst']:
 ref=data/'measurements/baseline'/f'{window}-t1-r0'
 ref_payload=payload(ref/'final.nscp')
 for version in ['baseline','optimized']:
  for run in sorted((data/'measurements'/version).glob(window+'-t*-r*')):
   if payload(run/'final.nscp')!=ref_payload:raise RuntimeError('Full checkpoint payload changed: '+str(run))
   checks.append(dict(check=str(run.relative_to(data))+' complete checkpoint payload',equal=True))
   for name in ['numbers.bin']+[p.name for p in ref.glob('result_*')]:equal(ref/name,run/name,str(run.relative_to(data))+' '+name)
 cross=compare.compare(ref/'final.nscp',data/'measurements/optimized'/f'{window}-t1-r0/final.nscp')
 if not cross['equal_except_build_metadata']:raise RuntimeError('Cross-build checkpoint failed')
 a.output.joinpath(window+'-checkpoint.json').write_text(json.dumps(cross,indent=2))
old=data/'baseline-release/network_studio_cli.exe';new=root/'build/Release/network_studio_cli.exe'
def cli(exe,name,args,expect=0):
 run=a.output/name;run.mkdir();cmd=[str(exe),'--output',str(run/'result'),'--checkpoint',str(run/'final.nscp')]+list(map(str,args))
 r=subprocess.run(cmd,capture_output=True,text=True);run.joinpath('process.log').write_text(r.stdout+r.stderr)
 if r.returncode!=expect:raise RuntimeError(f'{name}: {r.stderr}')
 return run
# Process exit/restart on the actual lab network, using each build's own NSCP.
for version,exe in [('baseline',old),('optimized',new)]:
 warm=data/'measurements'/version/'warmup/final.nscp'
 resumed=cli(exe,version+'-lab-resume',['--resume',warm,'--threads','12','--stop-at','.1'])
 direct=cli(exe,version+'-lab-direct',['--config',root.parent/'labs/lab6_config_initial.json','--threads','8','--stop-at','.1'])
 equal(resumed/'final.nscp',direct/'final.nscp',version+' lab cross-process resume 8->12')
 equal(direct/'final.nscp',data/'measurements'/version/'burst-t8-r0/final.nscp',version+' original CLI vs benchmark')
# Distinct small scientific fixtures, not modified laboratory experiments.
fixtures=[('delays',root/'examples/heterogeneous_delays.json',.08),('triplet',root/'examples/stdp_triplet.json',.25),('astro_hh',root/'examples/two_inputs.json',.1),('shared',root/'stimulation/examples/shared.json',.1),('spatial',root/'stimulation/examples/spatial.json',.1)]
for name,source,stop in fixtures:
 original=cli(old,name+'-old',['--config',source,'--stop-at',stop])
 updated=cli(new,name+'-new',['--config',source,'--stop-at',stop])
 result=compare.compare(original/'final.nscp',updated/'final.nscp')
 if not result['equal_except_build_metadata']:raise RuntimeError('Fixture changed: '+name)
 checks.append(dict(check=name+' complete checkpoint cross-build',equal=True))
 for file in original.glob('result_*'):equal(file,updated/file.name,name+' '+file.name)
# Incompatible build must be rejected by normal loader, without rewriting.
r=subprocess.run([str(new),'--package-probe','--resume',str(data/'measurements/baseline/warmup/final.nscp'),'--output',str(a.output/'must-not-exist.json')],capture_output=True,text=True)
if r.returncode==0 or (a.output/'must-not-exist.json').exists():raise RuntimeError('Compatibility check bypassed')
a.output.joinpath('incompatible-build.log').write_text(r.stdout+r.stderr)
# Read-only input provenance.
inputs=json.loads((data/'inputs.json').read_text())
for name,record in inputs.items():
 if digest(root.parent/'labs'/name)!=record['sha256']:raise RuntimeError('Lab input modified: '+name)
 checks.append(dict(check='unchanged labs/'+name,equal=True))
a.output.joinpath('result.json').write_text(json.dumps(dict(passed=True,checks=len(checks),details=checks,wall_seconds=time.perf_counter()-start),indent=2))
print('PASS',len(checks),'checks;',time.perf_counter()-start,'seconds')
