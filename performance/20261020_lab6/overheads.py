"""Separate solve, disk, transport and real Win32/DX11 measurements, serialized."""
import argparse,subprocess,json,time,hashlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2];config=root.parent/'labs/lab6_config_initial.json';exe=root/'build/Release';rows=[];start=time.perf_counter()
def launch(args,folder):
 begin=time.perf_counter();r=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 if not folder.exists():folder.mkdir(parents=True)
 folder.joinpath('process.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(str(args)+'\n'+r.stderr)
 return time.perf_counter()-begin
for repeat in range(3):
 for stop in [.1,.5]:
  for mode in (['rolling','archive'] if repeat%2==0 else ['archive','rolling']):
   folder=a.output/f'{mode}-{stop}-r{repeat}'
   wall=launch([exe/'network_studio_lab_benchmark.exe',config,folder,8,0,stop,mode],folder)
   row=json.loads(folder.joinpath('timing.json').read_text());row.update(mode=mode,repeat=repeat,total_wall_seconds=wall);rows.append(row)
   print(mode,stop,repeat,row['solve_seconds'],flush=True)
  left=a.output/f'rolling-{stop}-r{repeat}';right=a.output/f'archive-{stop}-r{repeat}'
  for f in ['numbers.bin']+[f.name for f in left.glob('result_*.csv')]:
   if (left/f).read_bytes()!=(right/f).read_bytes():raise RuntimeError('Recording changed '+f)
  if (right/'analysis_analysis.json').read_bytes()!=(right/'disk_analysis_analysis.json').read_bytes():raise RuntimeError('Disk/memory analysis differs')
 for hz in [10,20]:
  folder=a.output/f'runner-hz{hz}-r{repeat}'
  wall=launch([exe/'network_studio_runner_benchmark.exe',config,folder,hz,1,8,.1],folder)
  row=json.loads(folder.joinpath('benchmark.json').read_text());row.update(mode='runner',repeat=repeat,total_wall_seconds=wall);rows.append(row)
 folder=a.output/f'gui-r{repeat}'
 wall=launch([exe/'network_studio.exe',config,'--compute-threads',8,'--display-hz',10,'--benchmark',folder,'--benchmark-until',.1],folder)
 row=json.loads(folder.joinpath('benchmark.json').read_text());row.update(mode='gui',repeat=repeat,total_wall_seconds=wall);rows.append(row)
 # Authoritative checkpoints must be independent of publication/rendering.
 expected=(root/'performance/20261020_lab6/measurements/optimized/burst-t8-r0/final.nscp').read_bytes()
 for name in [f'runner-hz10-r{repeat}',f'runner-hz20-r{repeat}',f'gui-r{repeat}']:
  if (a.output/name/'final.nscp').read_bytes()!=expected:raise RuntimeError('GUI/transport changed checkpoint: '+name)
 a.output.joinpath('results.json').write_text(json.dumps(dict(results=rows,wall_seconds=time.perf_counter()-start),indent=2))
print('PASS: recording, disk analysis, runner and native GUI equality;',time.perf_counter()-start,'seconds')
