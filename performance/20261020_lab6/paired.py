"""AB/BA interleaved confirmation on the burst interval; never concurrent."""
import json,subprocess,time
from pathlib import Path
p=Path(__file__).resolve().parent;root=p.parents[1];out=p/'validation/paired';out.mkdir(exist_ok=False);rows=[];start=time.perf_counter()
for repeat in range(3):
 for mode in [8,16]:
  for version in (['baseline','optimized'] if repeat%2==0 else ['optimized','baseline']):
   exe=p/'baseline-release/network_studio_lab_benchmark.exe' if version=='baseline' else root/'build/Release/network_studio_lab_benchmark.exe'
   cp=p/'measurements'/version/'warmup/final.nscp';folder=out/f'{version}-t{mode}-r{repeat}'
   result=subprocess.run(list(map(str,[exe,cp,folder,mode,.05,.1])),text=True,capture_output=True)
   folder.joinpath('process.log').write_text(result.stdout+result.stderr)
   if result.returncode:raise RuntimeError(result.stderr)
   row=json.loads(folder.joinpath('timing.json').read_text());row.update(version=version,repeat=repeat);rows.append(row)
   print(version,mode,repeat,row['solve_seconds'],flush=True)
   ref=p/'measurements/baseline/burst-t8-r0/numbers.bin'
   if (folder/'numbers.bin').read_bytes()!=ref.read_bytes():raise RuntimeError('Numbers changed')
   out.joinpath('results.json').write_text(json.dumps(dict(results=rows,wall_seconds=time.perf_counter()-start),indent=2))

# Longer lab continuation: same build's strict checkpoint loader, not a converted state.
import compare_checkpoint as comparator
long_rows=[];long_start=time.perf_counter()
for version in ['baseline','optimized']:
 exe=p/'baseline-release/network_studio_lab_benchmark.exe' if version=='baseline' else root/'build/Release/network_studio_lab_benchmark.exe'
 cp=p/'measurements'/version/'burst-t8-r0/final.nscp';folder=out/f'long-{version}'
 result=subprocess.run(list(map(str,[exe,cp,folder,8,.1,.5])),text=True,capture_output=True)
 folder.joinpath('process.log').write_text(result.stdout+result.stderr)
 if result.returncode:raise RuntimeError(result.stderr)
 row=json.loads(folder.joinpath('timing.json').read_text());row['version']=version;long_rows.append(row)
 print('long',version,row['solve_seconds'],row['events'],flush=True)
a=out/'long-baseline';b=out/'long-optimized'
for f in ['numbers.bin']+[f.name for f in a.glob('result_*')]:
 if (a/f).read_bytes()!=(b/f).read_bytes():raise RuntimeError('Long continuation differs: '+f)
check=comparator.compare(a/'final.nscp',b/'final.nscp')
if not check['equal_except_build_metadata']:raise RuntimeError('Long checkpoint payload differs')
out.joinpath('long.json').write_text(json.dumps(dict(passed=True,results=long_rows,checkpoint=check,wall_seconds=time.perf_counter()-long_start),indent=2))
