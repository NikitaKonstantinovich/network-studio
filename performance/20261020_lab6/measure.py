"""Serial, repeated measurements: unmodified lab JSON, safe absolute stop boundaries."""
import argparse, json, subprocess, time, statistics, hashlib, platform, msvcrt
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('executable',type=Path);p.add_argument('output',type=Path);p.add_argument('--config',type=Path,default=Path('../labs/lab6_config_initial.json'));p.add_argument('--repeats',type=int,default=3)
a=p.parse_args()
lock=Path(__file__).with_name("measurement.lock").open("a+b");lock.seek(0)
try:msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
except OSError:raise RuntimeError("Another laboratory benchmark is already running")
a.output.mkdir(parents=True,exist_ok=False)
begin=time.perf_counter();rows=[];modes=[1,2,4,6,8,12,16,0]
def run(source,name,n,start,stop):
 out=a.output/name
 cp=subprocess.run([str(a.executable.resolve()),str(source.resolve()),str(out.resolve()),str(n),str(start),str(stop)],text=True,capture_output=True)
 out.joinpath('process.log').write_text(cp.stdout+cp.stderr)
 if cp.returncode:raise RuntimeError(cp.stderr)
 return json.loads(out.joinpath('timing.json').read_text())
run(a.config,'warmup',8,0,.05)
for repeat in range(a.repeats):
 order=modes[repeat:]+modes[:repeat]
 for mode in order:
  for window,start,stop in [('quiet',0,.05),('burst',.05,.1)]:
   source=a.config if window=='quiet' else a.output/'warmup/final.nscp'
   row=run(source,f'{window}-t{mode}-r{repeat}',mode,start,stop)
   row.update(window=window,repeat=repeat);rows.append(row)
   print(window,mode,repeat,round(row['solve_seconds'],5),flush=True)
   a.output.joinpath('measurements.json').write_text(json.dumps(rows,indent=2))
summary=[]
for window in ['quiet','burst']:
 for mode in modes:
  v=[r['solve_seconds'] for r in rows if r['window']==window and r['requested_threads']==mode]
  summary.append(dict(window=window,threads=mode,median=statistics.median(v),minimum=min(v),maximum=max(v)))
a.output.joinpath('summary.json').write_text(json.dumps(dict(results=summary,wall_seconds=time.perf_counter()-begin,repeats=a.repeats,executable=str(a.executable.resolve()),executable_sha256=hashlib.sha256(a.executable.read_bytes()).hexdigest(),config_sha256=hashlib.sha256(a.config.read_bytes()).hexdigest(),platform=platform.platform()),indent=2))
