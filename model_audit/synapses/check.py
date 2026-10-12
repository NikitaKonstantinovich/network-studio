"""Independent exact-flow and explicit-pair checks of the linked native harness.

No C++ recurrence or numerical integrator is reimplemented. Linear synaptic
flows use closed forms; STDP enumerates strictly older pre/post events.
"""
from __future__ import annotations
import csv, hashlib, json, math, statistics
from collections import defaultdict
from pathlib import Path
HERE=Path(__file__).resolve().parent
STUDIO=HERE.parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):
    with p.open(newline='',encoding='utf-8') as f:return list(csv.DictReader(f))
def save(p,d):p.write_text(json.dumps(d,indent=2,ensure_ascii=False,allow_nan=False)+'\n',encoding='utf-8')
def pair_step(c,w,time,pre,post,past_pre,past_post):
    positive=math.fsum(math.exp(-(time-t)/c['tau_plus']) for t in past_pre) if post else 0.
    negative=math.fsum(math.exp(-(time-t)/c['tau_minus']) for t in past_post) if pre else 0.
    gain_up=c['wmax']-w if c['rule']==1 else 1.
    gain_down=w-c['wmin'] if c['rule']==1 else 1.
    w=min(c['wmax'],max(c['wmin'],w+c['a_plus']*gain_up*positive-c['a_minus']*gain_down*negative))
    if pre:past_pre.append(time)
    if post:past_post.append(time)
    a=math.fsum(math.exp(-(time-t)/c['tau_plus']) for t in past_pre)
    b=math.fsum(math.exp(-(time-t)/c['tau_minus']) for t in past_post)
    return w,a,b

def main():
    configurations={r['case']:{k:float(v) for k,v in r.items() if k!='case'} for r in read(HERE/'raw/cases.csv')}
    grouped=defaultdict(list)
    for r in read(HERE/'raw/states.csv'):grouped[r['case']].append(r)
    result=dict(passed=False,state_cases={},current_checks={},pair_checks={},tail_examples=[])
    for name,rows in grouped.items():
        c=configurations[name];model=int(c['model']);u=.0 if model==1 else .31;x=1. if model==1 else .4;y=.2;Y=120.;w=c['weight']
        previous=0.;past_pre=[];past_post=[];max_errors={k:0. for k in ['u','x','s','Y','weight','pre_trace','post_trace','current']}
        min_resource=1.;max_x_plus_s=0.;events=0
        for index,r in enumerate(rows):
            time=float(r['time']);dt=time-previous;pre=bool(int(r['pre']));post=bool(int(r['post']));previous=time
            if index:
                if model!=1:
                    u*=math.exp(-dt/c['tau_f'])
                    er=math.exp(-dt/c['tau_d']);es=math.exp(-dt/c['tau_syn'])
                    if model==2:
                        if c['tau_d']==c['tau_syn']:convolution=y*dt/c['tau_d']*er
                        else:convolution=y*c['tau_syn']/(c['tau_syn']-c['tau_d'])*(es-er)
                        x=1+(x-1)*er-convolution
                    else:x=1+(x-1)*er
                y*=math.exp(-dt/c['tau_syn']);Y*=math.exp(-dt*c['omega_y'])
                if pre and c['enabled']:
                    events+=1
                    release=1.
                    if model!=1:
                        u+=c['U']*(1-u);release=u*x;x-=release
                    y+=release
                    excitatory=c['reversal']>-40 if model==0 else bool(c['stdp'] or c['weight']>0)
                    if excitatory:Y+=c['Y_release']*release
                if c['stdp'] and c['enabled']:
                    old_w=w;w,a,b=pair_step(c,w,time,pre,post,past_pre,past_post)
                    if post and not pre and abs(w-old_w)>1e-10 and len(result['tail_examples'])<4:
                        result['tail_examples'].append(dict(case=name,time_s=time,old_weight=old_w,new_weight=w,
                            tail_activity=y,instantaneous_efficacy_ratio=w/old_w,
                            meaning='A postsynaptic event changes the efficacy of an existing tail; no presynaptic release at this time.'))
                else:a=b=0.
            else:a=b=0.
            current=(c['conductance']*(c['reversal']+65) if model==0 else c['gain'])*w*y if c['enabled'] else 0.
            exact=dict(u=u,x=x,s=y,Y=Y,weight=w,pre_trace=a,post_trace=b,current=current)
            for field,expected in exact.items():
                actual=float(r['current_at_minus65'] if field=='current' else r[field]);assert math.isfinite(actual)
                max_errors[field]=max(max_errors[field],abs(actual-expected))
            max_x_plus_s=max(max_x_plus_s,float(r['x'])+float(r['s']))
            if model==2:min_resource=min(min_resource,float(r['x']),float(r['s']),1-float(r['x'])-float(r['s']))
        assert max(max_errors[k] for k in ['u','x','s'])<2e-6,(name,max_errors)
        assert max_errors['Y']<2e-3,(name,max_errors)
        assert max(max_errors[k] for k in ['weight','pre_trace','post_trace'])<2e-11,(name,max_errors)
        assert max_errors['current']<2e-7,(name,max_errors)
        if model==2:assert min_resource>=-1e-12,(name,min_resource)
        result['state_cases'][name]=dict(passed=True,parameters=c,rows=len(rows),presynaptic_releases=events,
            final_time_s=previous,maximum_absolute_errors=max_errors,minimum_TM_resource=min_resource if model==2 else None,
            maximum_x_plus_s=max_x_plus_s,
            current_and_weight_timing='Post-step state and live weight used; events grouped at step end; zero-lag mutual pair excluded.')
    currents=read(HERE/'raw/currents.csv');maximum=0.;by_target=defaultdict(float)
    for r in currents:
        v={k:float(x) for k,x in r.items()}
        expected=(v['g']*(v['E']-v['V']) if v['model']==0 else v['gain'])*v['weight']*v['s'] if v['enabled'] else 0.
        error=abs(v['current']-expected);maximum=max(maximum,error);by_target[int(v['neuron_model'])]=max(by_target[int(v['neuron_model'])],error)
        assert error<1e-8,(r,expected,error)
    result['current_checks']=dict(passed=True,cases=len(currents),maximum_absolute_error=maximum,by_target_error=dict(by_target),
        notes='HH recovered current divides voltage derivative by1000 and multiplies Cm=2.5; Izh current divides onlyby1000. Includes inhibitorysigns,zero weight,disabled,conductance reversal crossing.')
    groups=defaultdict(list)
    for row in read(HERE/'raw/pairs.csv'):groups[row['case']].append(row)
    for name,rows in groups.items():
        r=rows[0];c={k:float(r[src]) for k,src in dict(tau_plus='tauplus',tau_minus='tauminus',a_plus='Aplus',a_minus='Aminus',wmin='wmin',wmax='wmax',rule='rule').items()}
        w=float(r['initial']);pre_history=[];post_history=[];errors=[0.,0.,0.];bounds=set();same_step=0
        for r in rows:
            t=float(r['time']);pre=bool(int(r['pre']));post=bool(int(r['post']));same_step+=pre and post
            w,a,b=pair_step(c,w,t,pre,post,pre_history,post_history)
            for i,(key,expected) in enumerate([('weight',w),('pre_trace',a),('post_trace',b)]):errors[i]=max(errors[i],abs(float(r[key])-expected))
            if w in [c['wmin'],c['wmax']]:bounds.add(w)
        assert max(errors)<2e-11,(name,errors)
        result['pair_checks'][name]=dict(passed=True,updates=len(rows),maximum_errors=errors,
            simultaneous_groups=same_step,bounds_visited=sorted(bounds),final_weight=w)
    result['convergence']={}
    for name,r in result['state_cases'].items():
        if name.endswith('dt50'):
            fine=result['state_cases'][name[:-4]+'dt25'];coarse=max(r['maximum_absolute_errors'][k] for k in ['u','x','s']);small=max(fine['maximum_absolute_errors'][k] for k in ['u','x','s'])
            result['convergence'][name[:-4]]=dict(coarse_error=coarse,fine_error=small,ratio=coarse/small if small else None)
    result['passed']=True
    result['provenance']=dict(source_sha256={str(p.relative_to(STUDIO)):sha(p) for p in [STUDIO/'network.cpp',STUDIO/'network.h',STUDIO/'README.md',STUDIO/'gui.cpp']},
        linked_library_sha256=sha(STUDIO/'build/native/network_core.lib'),production_cli_sha256=sha(STUDIO/'build/Release/network_studio_cli.exe'),
        harness_sha256=sha(HERE/'probe.exe'),scripts_sha256={p.name:sha(p) for p in HERE.iterdir() if p.suffix in ['.cpp','.py','.ps1']},
        data_sha256={p.name:sha(p) for p in (HERE/'raw').glob('*.csv')})
    save(HERE/'results.json',result)
    print('PASS',len(configurations),'synapse cases;',sum(r['rows'] for r in result['state_cases'].values()),'states;',len(currents),'current probes;',sum(r['updates'] for r in result['pair_checks'].values()),'pair updates')
    print('Maximum errors:',{k:max(c['maximum_absolute_errors'][k] for c in result['state_cases'].values()) for k in ['u','x','s','Y','weight','pre_trace','post_trace','current']})

if __name__=='__main__':main()
