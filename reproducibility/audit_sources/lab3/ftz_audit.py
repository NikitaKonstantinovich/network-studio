"""Separate FTZ+DAZ diagnostic; never replaces production results."""
from __future__ import annotations
import argparse, concurrent.futures, csv, datetime, json, subprocess, sys, time
from pathlib import Path
sys.dont_write_bytecode=True
import audit as a
import network_audit as n

FTZ=a.HERE/'ftz/build/Release/network_studio_cli_ftz.exe'

def compare(original,diagnostic):
    out={}
    for kind in ['events','weights','trace']:
        old=a.rawpath(original,kind+'.csv');new=a.rawpath(diagnostic,kind+'.csv')
        if not old.exists():out[kind]={'available':False};continue
        sha_equal=a.sha(old)==a.sha(new)
        result={'available':True,'bytewise_equal':sha_equal,'original_sha256':a.sha(old),'ftz_sha256':a.sha(new)}
        if kind=='events':
            result['exact_event_order_and_times_equal']=old.read_bytes()==new.read_bytes()
        else:
            columns={};rows=0
            with old.open(newline='',encoding='utf-8') as fo,new.open(newline='',encoding='utf-8') as fn:
                ro=csv.DictReader(fo);rn=csv.DictReader(fn);assert ro.fieldnames==rn.fieldnames
                for name in ro.fieldnames:columns[name]={'different_values':0,'max_absolute_difference':0.,'max_normal_value_difference':0.,'max_subnormal_value_difference':0.}
                for xo,xn in zip(ro,rn,strict=True):
                    rows+=1
                    for key in ro.fieldnames:
                        vo=float(xo[key]);vn=float(xn[key]);diff=abs(vo-vn)
                        st=columns[key];st['different_values']+=vo!=vn
                        st['max_absolute_difference']=max(st['max_absolute_difference'],diff)
                        cat='max_normal_value_difference' if max(abs(vo),abs(vn))>=sys.float_info.min else 'max_subnormal_value_difference'
                        st[cat]=max(st[cat],diff)
            result.update(rows=rows,columns=columns,all_values_numerically_equal=all(x['different_values']==0 for x in columns.values()),
                          max_absolute_difference=max(x['max_absolute_difference'] for x in columns.values()),
                          max_normal_value_difference=max(x['max_normal_value_difference'] for x in columns.values()),
                          max_subnormal_value_difference=max(x['max_subnormal_value_difference'] for x in columns.values()))
        out[kind]=result
    return out

def run_case(original,skip=False):
    name='ftz_'+original;config=a.read(a.HERE/'configs'/f'{original}.json')
    elapsed=None
    if not skip:
        start=time.perf_counter()
        command=[str(FTZ),'--config',str(a.HERE/'configs'/f'{original}.json'),'--output',str(a.HERE/'raw'/name)]
        child=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        print('FTZ START',original,'pid',child.pid,datetime.datetime.now().isoformat(),flush=True)
        stdout,stderr=child.communicate()
        a.rawpath(name,'cli.log').write_text(stdout+stderr,encoding='utf-8')
        if child.returncode:raise RuntimeError(stderr+stdout)
        elapsed=time.perf_counter()-start
        assert 'FTZ+DAZ enabled' in stderr
    summary=a.read(a.rawpath(name,'summary.json'));assert summary['complete'] and not summary['error']
    assert a.compat.canonical_config(config)==a.compat.canonical_config(a.read(a.rawpath(name,'experiment.json')))
    row=dict(name=name,original_name=original,mode='FTZ+DAZ MXCSR bits 15 and 6',cli_sha256=a.sha(FTZ),wall_s=elapsed,
             config_sha256=a.sha(a.HERE/'configs'/f'{original}.json'),total_spikes=sum(summary['neuron_spikes']),comparison=compare(original,name))
    if config['duration']>.2:
        source={'network_net1_0p1Hz':'net1_0.1Hz.net','network_net1_1Hz':'net1_1Hz.net','network_net1_100Hz':'net1_100Hz.net'}[original]
        _,info=n.import_network(n.SOURCE/source)
        row['network_metrics']=n.analyze(name,config,info,elapsed)
    print('FTZ DONE',original,'wall_s',elapsed,'spikes',row['total_spikes'],flush=True)
    return row

def main():
    p=argparse.ArgumentParser();p.add_argument('--skip-run',action='store_true');args=p.parse_args()
    output=a.HERE/'ftz_findings.json'
    old={r['name']:r for r in a.read(output)['runs']} if output.exists() else {}
    result=dict(scope=__doc__,production_cli_sha256=a.sha(a.CLI),ftz_cli_sha256=a.sha(FTZ),
                linked_unchanged_network_core_sha256=a.sha(a.STUDIO/'build/native/network_core.lib'),
                source_sha256={x:a.sha(a.STUDIO/x) for x in ['cli.cpp','network.cpp','io.cpp','network.h']},
                launcher_sha256=a.sha(a.HERE/'ftz/launcher.cpp'),script_sha256=a.sha(__file__),
                compiler='MSVC 19.44.35217.0, x64 Release, /O2 /Ob2 /MT, same existing static core',
                convention='Set MXCSR FTZ and DAZ before original CLI entry. Flush subnormal arithmetic results and operands to zero. No equation, timestep, or model changes.',runs=[])
    cases=['network_benchmark','network_net1_100Hz','network_net1_1Hz','network_net1_0p1Hz']
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures={pool.submit(run_case,x,args.skip_run):x for x in cases}
        for future in concurrent.futures.as_completed(futures):
            row=future.result()
            if row['wall_s'] is None and row['name'] in old:
                row['wall_s']=old[row['name']]['wall_s']
                if 'network_metrics' in row:row['network_metrics']['wall_s']=row['wall_s']
            result['runs'].append(row);a.save(output,result)
    assert a.sha(a.CLI)==result['production_cli_sha256']
    assert a.sha(FTZ)==result['ftz_cli_sha256']
    print('FTZ AUDIT COMPLETE',flush=True)

if __name__=='__main__':main()
