"""Read-only-source Lab 3 audit. All generated output stays beside this script."""
from __future__ import annotations
import argparse, bisect, copy, csv, hashlib, importlib.util, json, math, statistics, subprocess, sys, time
from pathlib import Path
sys.dont_write_bytecode=True

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[2]
LAB = STUDIO / 'lab3'
CLI = STUDIO / 'build/Release/network_studio_cli.exe'
FREQS = [.1,.5,1.,5.,10.,25.,50.,100.]

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path); obj=importlib.util.module_from_spec(spec); spec.loader.exec_module(obj); return obj
tm=module('lab3_tm_reference',LAB/'reference_tm.py')
compat=module('lab3_config_compat',STUDIO/'lab1/config_compat.py')
extract=module('lab3_neuronet_extract',LAB/'extract_neuronet_reference.py')
def read(path): return json.loads(Path(path).read_text(encoding='utf-8'))
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):
    path=Path(path); path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
def rawpath(name,kind): return HERE/'raw'/f'{name}_{kind}'

def run(name,c,skip=False):
    path=HERE/'configs'/f'{name}.json'; save(path,c)
    if not skip:
        start=time.perf_counter()
        p=subprocess.run([str(CLI),'--config',str(path),'--output',str(HERE/'raw'/name)],capture_output=True,text=True)
        rawpath(name,'cli.log').write_text(p.stdout+p.stderr,encoding='utf-8')
        if p.returncode: raise RuntimeError(f'{name}: {p.stdout}{p.stderr}')
        elapsed=time.perf_counter()-start
    else: elapsed=None
    snapshot=read(rawpath(name,'experiment.json'))
    assert compat.canonical_config(c)==compat.canonical_config(snapshot),(name,'snapshot mismatch')
    summary=read(rawpath(name,'summary.json'))
    assert summary['complete'] and not summary['error'] and abs(summary['time_s']-c['duration'])<1e-9,(name,summary)
    with rawpath(name,'events.csv').open(encoding='utf-8',newline='') as f: events=list(csv.DictReader(f))
    ev=[float(row['time_s']) for row in events if row['node_index_1based']=='1' and row['type']=='spike']
    assert len(ev)==summary['neuron_spikes'][0]
    return ev,summary,elapsed

def periods(ev,res,c):
    f=c['neurons'][0]['frequency']; start=c['neurons'][0]['start']
    first=max(2,math.ceil(5*f)); last=math.floor((c['duration']-start)*f+1e-8)
    assert last>first,(first,last)
    counts=[]; peaks=[]; jumps=[]
    for k in range(first,last):
        rows=[r for r in res if start+k/f<=r['time_s']<start+(k+1)/f]
        counts.append(len(rows))
        if rows: peaks.append(max(r['peak'] for r in rows)); jumps.append(max(r['jump'] for r in rows))
    return dict(steady_window_s=[start+first/f,start+last/f],periods=len(counts),responding_periods=len(peaks),
                spike_rate_hz=statistics.mean(counts)*f,spikes_per_period_mean=statistics.mean(counts),
                spikes_per_period_min=min(counts),spikes_per_period_max=max(counts),
                peak_mean=statistics.mean(peaks) if peaks else None,peak_min=min(peaks) if peaks else None,
                peak_max=max(peaks) if peaks else None,jump_mean=statistics.mean(jumps) if jumps else None,
                first_peak=res[0]['peak'] if res else None,first_spike_time_s=ev[0] if ev else None)

def analyze(name,c,events,summary,elapsed,oldname=None):
    syn=c['synapses'][0]
    kw=dict(U=syn['release'],tau_fac=syn['tau_f'],tau_rec=syn['tau_d'],tau_syn=syn['tau_syn'],gain=syn['gain'])
    responses=tm.tm_responses(events,**kw)
    rows=[]
    with rawpath(name,'trace.csv').open(encoding='utf-8',newline='') as f:
        for row in csv.DictReader(f): rows.append([float(row[k]) for k in ['time_s','S1_u','S1_x','S1_y']])
    ref=tm.state_at(events,[r[0] for r in rows],**kw)
    errors={k:max(abs(r[i+1]-a[k]) for r,a in zip(rows,ref)) for i,k in enumerate(['u','x','y'])}
    assert max(errors.values())<1e-8,(name,errors)
    assert all(-1e-9<=r[1]<=1+1e-9 and r[2]>=-1e-9 and r[3]>=-1e-9 and r[2]+r[3]<=1+1e-9 for r in rows)
    assert summary['final_synapse_weights']==[syn['weight']]
    result=dict(name=name,config_sha256=sha(HERE/'configs'/f'{name}.json'),rows=len(rows),wall_s=elapsed,
                exact_tm_max_abs_errors=errors,neuron_spikes=summary['neuron_spikes'],metrics=periods(events,responses,c),
                csv_sha256={k:sha(rawpath(name,k+'.csv')) for k in ['trace','events','weights']})
    if oldname:
        result['old_csv_bytewise_equal']={k:sha(LAB/'results/raw'/f'{oldname}_{k}.csv')==result['csv_sha256'][k] for k in ['trace','events','weights']}
        result['old_snapshot_normalized_equal']=compat.canonical_config(read(LAB/'results/raw'/f'{oldname}_experiment.json'))==compat.canonical_config(c)
    return result

def short_config(f,amplitude,width):
    c=read(LAB/'configs/f100.json'); c.update(name=f'Calibration {f:g}Hz {amplitude:g}x{width*1000:g}ms',duration=3,sample=.001)
    for i,n in enumerate(c['neurons']): n.update(start=.1,stop=3.,frequency=f,amplitude=amplitude if i==0 else 0.,width=width)
    return c

def calibrate(skip):
    result=[]; selected={}
    for f in FREQS:
        if f<25: selected[f]=(30.,.001); continue
        # Search increasing pulse charge; require exactly one spike in EVERY
        # steady stimulus period, rather than merely matching a mean rate.
        candidates=sorted({(a,w) for a in [30.,40.,50.,60.,80.,100.] for w in [.001,.002,.003,.004]},key=lambda t:(t[0]*t[1],t[0],t[1]))
        for index,(amp,width) in enumerate(candidates):
            c=short_config(f,amp,width); name=f'calibration_f{f:g}_{index}'.replace('.','p')
            ev,summary,elapsed=run(name,c,skip)
            begin=1.1; end=2.9
            counts=[bisect.bisect_left(ev,begin+(k+1)/f)-bisect.bisect_left(ev,begin+k/f) for k in range(round((end-begin)*f))]
            row=dict(frequency_hz=f,amplitude=amp,width_ms=width*1000,min_spikes=min(counts),max_spikes=max(counts),name=name,wall_s=elapsed)
            result.append(row)
            if min(counts)==max(counts)==1: selected[f]=(amp,width); break
        if f not in selected: raise AssertionError(f'Unable to calibrate {f:g}Hz')
        print('CALIBRATED',f,selected[f],flush=True)
    return result,selected

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--skip-run',action='store_true');args=parser.parse_args()
    (HERE/'raw').mkdir(parents=True,exist_ok=True)
    result=dict(cli_path=str(CLI),cli_sha256=sha(CLI),reference_self_test=tm.self_test(),historical=[],calibrated=[],convergence=[],sources={})
    for path in [LAB/'reference_tm.py',LAB/'compare.py',LAB/'neuronet_snapshot_reference.json',LAB/'extract_neuronet_reference.py',STUDIO/'network.cpp',STUDIO/'lab1/config_compat.py',extract.DEFAULT_PDF]:
        result['sources'][str(path)]=sha(path)
    paths=sorted((LAB/'configs').glob('*.json')); assert len(paths)==17
    for path in paths:
        c=read(path); name='historical_'+path.stem
        ev,summary,elapsed=run(name,c,args.skip_run)
        row=analyze(name,c,ev,summary,elapsed,path.stem);result['historical'].append(row)
        print('HISTORICAL',path.stem,row['old_csv_bytewise_equal'],f'{elapsed or 0:.2f}s',flush=True)
        save(HERE/'task1_progress.json',result)
    trials,selected=calibrate(args.skip_run);result['calibration_trials']=trials
    for f in FREQS:
        stem=f'f{f:g}'.replace('.','p');c=read(LAB/'configs'/f'{stem}.json')
        amp,width=selected[f];c['name']=f'Lab3 calibrated {f:g}Hz: {amp:g} x {width*1000:g}ms'
        c['neurons'][0].update(amplitude=amp,width=width)
        name='calibrated_'+stem;ev,summary,elapsed=run(name,c,args.skip_run)
        row=analyze(name,c,ev,summary,elapsed)
        assert row['metrics']['spikes_per_period_min']==row['metrics']['spikes_per_period_max']==1,(name,row['metrics'])
        ref=tm.regular_response(f);row.update(frequency_hz=f,amplitude=amp,width_ms=width*1000,regular_tm_reference=ref,
                  relative_peak_error=abs(row['metrics']['peak_mean']/ref['peak']-1))
        result['calibrated'].append(row)
        print('CALIBRATED FULL',f,row['metrics']['spike_rate_hz'],row['metrics']['peak_mean'],flush=True)
        if f in [5.,50.,100.]:
            fine=copy.deepcopy(c);fine['dt']/=2
            ev2,summary2,elapsed2=run(name+'_fine',fine,args.skip_run)
            fine_row=analyze(name+'_fine',fine,ev2,summary2,elapsed2)
            fine_row.update(frequency_hz=f,peak_relative_change=abs(fine_row['metrics']['peak_mean']/row['metrics']['peak_mean']-1),
                max_paired_spike_time_change_ms=max(abs(a-b)*1000 for a,b in zip(ev,ev2)) if len(ev)==len(ev2) else None)
            result['convergence'].append(fine_row)
        save(HERE/'task1_progress.json',result)
    old=read(LAB/'neuronet_snapshot_reference.json')
    fresh=extract.extract(extract.DEFAULT_SOURCE,extract.DEFAULT_PDF,189)
    save(HERE/'fresh_neuronet_snapshot_reference.json',fresh)
    for key in old:
        if key!='created_utc': assert old[key]==fresh[key],key
    # Recompute raw recurrence residuals directly from original per-snapshot fields.
    records=fresh['records']; nonzero=[r for r in records if r['raw']['Output']>0]
    residuals={k:0. for k in ['Output','parameter_u','parameter_x','parameter_z']}
    for prev,cur in zip(nonzero,nonzero[1:]):
        d=(cur['time_s']-prev['time_s'])*1000; p=prev['raw'];q=cur['raw']
        predicted={'Output':p['Output']*(1-d/10),'parameter_u':p['parameter_u']*(1-d/1000),
                   'parameter_x':p['parameter_x']+d*p['parameter_z']/50,
                   'parameter_z':p['parameter_z']+d*(p['Output']/10-p['parameter_z']/50)}
        for k in residuals: residuals[k]=max(residuals[k],abs(q[k]-predicted[k]))
    result['external_snapshot_check']=dict(records=len(records),original_files_hashes_match=True,first_output=fresh['first_observed_response'],
        residuals_raw_units=residuals,available_interval_s=fresh['available_interval_s'])
    result['bytewise_equal_count']=sum(sum(row['old_csv_bytewise_equal'].values()) for row in result['historical'])
    result['historical_rows']=sum(row['rows'] for row in result['historical'])
    result['analytical_no_facilitation_limit_only']=[tm.regular_response(f,tau_fac=0) for f in FREQS]
    result['script_sha256']=sha(__file__)
    assert sha(CLI)==result['cli_sha256'],'Executable changed during audit'
    save(HERE/'task1_findings.json',result)
    print('TASK1 COMPLETE',result['bytewise_equal_count'],'of51 CSVs; rows',result['historical_rows'],flush=True)

if __name__=='__main__': main()
