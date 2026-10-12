"""Independent neuron audit; writes only beside this file, never builds production.

Reference uses milliseconds, original HH voltage convention, adaptive SciPy
DOP853/Radau and continuous event location. No production RHS is imported.
"""
from __future__ import annotations
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess

import numpy as np
import scipy
from scipy.integrate import solve_ivp
from scipy.optimize import root_scalar

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[1]
CLI = STUDIO / 'build/Release/network_studio_cli.exe'
EXPECTED_CLI_SHA = '4965bb5cc41f13f243bf9823f9c3d6c68c1ab0a77d1677574007003f50342a8e'

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')

def hh_rates(voltage):
    # Original paper's relative voltage has the opposite sign: W = -(V+65).
    w = -(voltage + 65)
    def quotient(z):
        return 1.0 if z == 0 else z / math.expm1(z)
    alpha = np.array([quotient((w+25)/10), .07*math.exp(w/20), .1*quotient((w+10)/10)])
    beta = np.array([4*math.exp(w/18), 1/(math.exp((w+30)/10)+1), .125*math.exp(w/80)])
    return alpha, beta

def hh_rhs_ms(state, current, p):
    v, m, h, n = state
    a, b = hh_rates(v)
    ionic = p['g_Na']*m**3*h*(v-p['E_Na']) + p['g_K']*n**4*(v-p['E_K']) + p['g_L']*(v-p['E_L'])
    return np.r_[(current-ionic)/p['C_m'], a*(1-state[1:])-b*state[1:]]

def initial(config):
    n = config['neurons'][0]
    if n['model'] == 'izhikevich':
        return np.array([n['voltage'], n['izh_b']*n['voltage'] if n['equilibrium_u'] else n['izh_u']])
    a, b = hh_rates(n['voltage'])
    return np.r_[n['voltage'], a/(a+b)]

def reference(config, method='DOP853', override_initial=None):
    n, p = config['neurons'][0], config['parameters']
    izh = n['model'] == 'izhikevich'
    end = 1000*config['duration']
    # The fixture uses either DC or one finite rectangular pulse. Split exactly
    # at its edges, so adaptive stepping cannot miss a short pulse.
    boundaries = sorted(set([0., end] + [t for t in [1000*n['start'],1000*(n['start']+n['width']),1000*n['stop']] if 0<t<end]))
    state = initial(config) if override_initial is None else np.array(override_initial)
    segments, events, resets = [], [], []
    for left, right in zip(boundaries[:-1],boundaries[1:]):
        mid=(left+right)/2000
        current=n['current']
        if n['amplitude'] and n['frequency']>0 and n['start']<=mid<n['stop'] and mid-n['start']<n['width']:
            current += n['amplitude']
        def fun(t,y):
            if izh:
                v,u=y
                return [.04*v*v+5*v+140-u+current,n['izh_a']*(n['izh_b']*v-u)]
            return hh_rhs_ms(y,current,p)
        def spike(t,y): return y[0]-(n['izh_peak'] if izh else p['V_spike'])
        spike.direction=1
        spike.terminal=izh
        cursor=left
        while cursor<right-1e-13:
            sol=solve_ivp(fun,(cursor,right),state,method=method,rtol=2e-12,atol=2e-13,
                          max_step=.05,dense_output=True,events=spike)
            if not sol.success: raise RuntimeError(sol.message)
            segments.append((cursor,float(sol.t[-1]),sol.sol))
            events.extend(sol.t_events[0].tolist())
            state=sol.y[:,-1].copy()
            cursor=float(sol.t[-1])
            if izh and len(sol.t_events[0]):
                before=state.tolist()
                state=np.array([n['izh_c'],state[1]+n['izh_d']])
                resets.append({'time_ms':cursor,'before':before,'after':state.tolist()})
    def sample(times_s):
        times=np.array(times_s)*1000
        output=np.zeros((len(state),len(times)))
        for left,right,dense in segments:
            mask=(times>=left-1e-10)&(times<=right+1e-10)
            output[:,mask]=dense(np.clip(times[mask],left,right))
        if izh:
            for r in resets:
                output[:,np.isclose(times,r['time_ms'],rtol=0,atol=1e-10)]=np.array(r['after'])[:,None]
        return output
    return {'event_times_ms':events,'resets':resets,'final_state':state.tolist(),'sample':sample}

def native(config,name):
    directory=HERE/'runs';directory.mkdir(exist_ok=True)
    filename=directory/(name+'.json');prefix=directory/name
    write_json(filename,config)
    result=subprocess.run([str(CLI),'--config',str(filename),'--output',str(prefix)],capture_output=True,text=True)
    (directory/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
    if result.returncode: raise RuntimeError(name+': '+result.stderr)
    data=np.genfromtxt(str(prefix)+'_trace.csv',delimiter=',',names=True)
    events=[]
    with open(str(prefix)+'_events.csv',encoding='utf-8') as stream:
        for row in csv.DictReader(stream):events.append(float(row['time_s'])*1000)
    summary=json.loads(Path(str(prefix)+'_summary.json').read_text(encoding='utf-8'))
    assert summary['complete']
    columns=data.dtype.names
    values=np.array([data[col] for col in columns[1:5]])
    assert np.isfinite(values).all()
    return data[columns[0]],values,events,summary

def point_checks(base):
    rows=list(csv.DictReader((HERE/'probe.csv').open(encoding='utf-8')))
    hh_error,gate_error,izh_error,float_scaled_error=0.,0.,0.,0.
    singular=[]
    for row in rows:
        v=float(row['voltage'])
        actual=np.array([float(row[k]) for k in ('dV','dm','dh','dn')])
        if row['case']=='hh':
            expect=1000*hh_rhs_ms(np.array([v,.2,.4,.6]),7.3,base['parameters'])
            hh_error=max(hh_error,float(np.max(abs(expect-actual))))
            a,b=hh_rates(v)
            gates=np.array([float(row[k]) for k in ('initial_m','initial_h','initial_n')])
            gate_error=max(gate_error,float(np.max(abs(a/(a+b)-gates))))
            f=np.array([float(row[k]) for k in ('float_dV','float_dm','float_dh','float_dn')])
            float_scaled_error=max(float_scaled_error,float(np.max(abs(f-expect)/np.maximum(1,abs(expect)))))
            if v in (-40,-55):singular.append({'v_mV':v,'alpha_m_per_ms':float(a[0]),'alpha_n_per_ms':float(a[2]),'all_rhs_finite':bool(np.isfinite(actual).all())})
        else:
            expected=np.array([1000*(.04*v*v+5*v+140+13+7.3),1000*.02*(.2*v+13),0,0])
            izh_error=max(izh_error,float(np.max(abs(expected-actual))))
    assert hh_error<1e-8 and gate_error<1e-14 and izh_error<1e-9 and float_scaled_error<1e-5
    return {'hh_voltage_points':16,'izh_voltage_points':7,'hh_max_rhs_abs_error_per_s':hh_error,
            'hh_max_initial_gate_abs_error':gate_error,'izh_max_rhs_abs_error_per_s':izh_error,
            'float_max_scaled_rhs_error':float_scaled_error,'singularities':singular}

def main():
    before=sha(CLI);assert before==EXPECTED_CLI_SHA
    base=json.loads((HERE/'base.json').read_text(encoding='utf-8'))
    n=base['neurons'][0];n.update(amplitude=0,current=0,start=.005,stop=.006,width=.001,frequency=1)
    base['duration']=.1;base['sample']=.00005
    scenarios=[]
    for name,current,voltage,cm,duration in [
        ('hh_rest',0,-65,1,.1),('hh_dc4',4,-65,1,.1),('hh_dc10',10,-65,1,.1),
        ('hh_dc100',100,-65,1,.05),('hh_cm2',10,-65,2,.1),
        ('hh_initial_minus55',0,-55,1,.02),('hh_initial_minus40',0,-40,1,.02),
        ('hh_initial_minus100',0,-100,1,.02),('hh_initial_plus60',0,60,1,.02),
        ('hh_initial_minus50',0,-50,1,.02),('hh_pulse10_1ms',0,-65,1,.05)]:
        c=copy.deepcopy(base);c['duration']=duration;c['parameters']['C_m']=cm
        c['neurons'][0].update(current=current,voltage=voltage)
        if name=='hh_pulse10_1ms':c['neurons'][0]['amplitude']=10
        scenarios.append((name,c))
    for name,a,b,cval,d in [('RS',.02,.2,-65,8),('IB',.02,.2,-55,4),('CH',.02,.2,-50,2),('FS',.1,.2,-65,2),('LTS',.02,.25,-65,2)]:
        c=copy.deepcopy(base);c['duration']=.2
        c['neurons'][0].update(model='izhikevich',current=10,izh_a=a,izh_b=b,izh_c=cval,izh_d=d)
        scenarios.append(('izh_'+name,c))
    c=copy.deepcopy(base);c['duration']=.001;c['sample']=.000001
    c['neurons'][0].update(model='izhikevich',voltage=29.9,equilibrium_u=False,izh_u=-13)
    scenarios.append(('izh_near_peak',c))
    checks=[];reference_info=[]
    refs={}
    for name,c in scenarios:
        ref=reference(c);refs[name]=ref
        info={k:v for k,v in ref.items() if k!='sample'};info['name']=name
        reference_info.append(info)
        steps=[50e-6,25e-6,12.5e-6,5e-6] if name in ('hh_rest','hh_dc10','hh_pulse10_1ms') else [25e-6,12.5e-6,5e-6]
        for dt in steps:
            run=copy.deepcopy(c);run['dt']=dt;run['sample']=max(run['sample'],dt)
            key=name+'_dt'+str(dt*1e6).replace('.','p')+'us'
            times,values,events,summary=native(run,key)
            count_equal=len(events)==len(ref['event_times_ms'])
            timing=max((abs(a-b) for a,b in zip(events,ref['event_times_ms'])),default=0) if count_equal else None
            row={'name':name,'dt_us':dt*1e6,'native_count':len(events),'reference_count':len(ref['event_times_ms']),
                 'counts_equal':count_equal,'max_event_error_ms':timing,'samples':len(times),
                 'gates_in_unit_interval':None,'final_native_state':values[:,-1].tolist()}
            reference_values=ref['sample'](times)
            if name.startswith('hh'):
                row.update(max_voltage_error_mV=float(np.max(abs(values[0]-reference_values[0]))),
                           max_gate_error=float(np.max(abs(values[1:]-reference_values[1:]))),
                           gates_in_unit_interval=bool(((values[1:]>=0)&(values[1:]<=1)).all()),
                           recorded_peak_mV=float(max(values[0])))
            if name=='izh_near_peak':
                row['first_step_u']=float(values[1,1]);row['first_step_voltage']=float(values[0,1])
                row['reference_first_reset']=ref['resets'][0]
                row['u_at_same_time_reference']=float(reference_values[1,1])
            checks.append(row)
        print(name, 'reference events:',len(ref['event_times_ms']),flush=True)
    cross=[]
    for name in ('hh_dc10','hh_pulse10_1ms','izh_FS','izh_near_peak'):
        c=dict(scenarios)[name];radau=reference(c,method='Radau');dop=refs[name]
        grid=np.linspace(0,c['duration'],1001)
        cross.append({'name':name,'counts_equal':len(radau['event_times_ms'])==len(dop['event_times_ms']),
                      'max_event_error_ms':max((abs(a-b) for a,b in zip(radau['event_times_ms'],dop['event_times_ms'])),default=0),
                      'max_sample_abs_difference':float(np.max(abs(radau['sample'](grid)-dop['sample'](grid))))})
    # A membrane instantaneous-voltage shock keeps gates at resting values;
    # Studio instead initializes each gate to equilibrium at the chosen voltage.
    shock=copy.deepcopy(dict(scenarios)['hh_initial_minus50'])
    a,b=hh_rates(-65)
    shock_ref=reference(shock,override_initial=np.r_[-50.,a/(a+b)])
    standard_ref=refs['hh_initial_minus50']
    grid=np.linspace(0,.02,2001)
    initial_semantics={'voltage_mV':-50,'equilibrium_at_selected_voltage_events':len(standard_ref['event_times_ms']),
        'equilibrium_at_selected_voltage_peak_mV':float(standard_ref['sample'](grid)[0].max()),
        'instantaneous_shock_from_rest_events':len(shock_ref['event_times_ms']),
        'instantaneous_shock_from_rest_peak_mV':float(shock_ref['sample'](grid)[0].max()),
        'comment':'Different initial conditions, not an equation bug; HH gate initial values are not exposed by Config.'}
    def equilibrium_voltage(v):
        a,b=hh_rates(v)
        return hh_rhs_ms(np.r_[v,a/(a+b)],0,base['parameters'])[0]
    rest=root_scalar(equilibrium_voltage,bracket=(-66,-64),xtol=1e-12).root
    prior=STUDIO/'verification/repro_labs_20261012/lab1/findings.json'
    prior_data=json.loads(prior.read_text(encoding='utf-8'))
    result={'scope':'Neuron equations, units, HH singularities and initial conditions, deterministic integration and event reset; not full biological or coupled-network validation.',
        'provenance':{'cli_sha256':before,'unchanged_after':sha(CLI)==before,'network_core_sha256':sha(STUDIO/'build/native/network_core.lib'),
            'source_sha256':{str(p.relative_to(STUDIO)):sha(p) for p in [STUDIO/'network.cpp',STUDIO/'network.h',STUDIO/'vendor/systems/neuro_astro/model.cuh']},
            'numpy':np.__version__,'scipy':scipy.__version__},
        'primary_sources':[
            {'title':'Izhikevich 2003, Simple Model of Spiking Neurons','url':'https://www.izhikevich.org/publications/spikes.pdf','location':'equations (1)-(3), p1569; Fig2 and original discrete code pp1570-1571'},
            {'title':'Hodgkin and Huxley 1952','url':'https://materias.df.uba.ar/dnla2019c1/files/2019/03/hodgkin1952.pdf','doi':'10.1113/jphysiol.1952.sp004764','location':'pp518-520 equations (7),(12),(13),(15),(16),(20),(21),(23),(24),(26), Table3; p522 initial voltage shock'}],
        'existing_izh_audit':{'path':str(prior),'sha256':sha(prior),'already_completed_counts':prior_data['counts'],'rerun_here':False},
        'point_checks':point_checks(base),'template_core_bridge':json.loads((HERE/'probe_bridge.json').read_text(encoding='utf-8-sig')),
        'new_cli_runs':len(checks),'distinct_DOP853_cases':len(scenarios),'Radau_crosschecks':cross,'checks':checks,
        'hh_rest':{'true_equilibrium_voltage_mV':rest,'initial_dV_mV_per_ms':equilibrium_voltage(-65)},
        'HH_initial_voltage_semantics':initial_semantics}
    write_json(HERE/'reference.json',reference_info)
    write_json(HERE/'findings.json',result)
    print(json.dumps({'runs':len(checks),'count_agreement':all(r['counts_equal'] for r in checks),'probe':result['point_checks'],
                      'HH_initial_voltage_semantics':initial_semantics,'hh_rest':result['hh_rest']},indent=2))

if __name__=='__main__':main()
