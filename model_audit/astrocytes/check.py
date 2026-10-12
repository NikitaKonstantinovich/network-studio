"""Independent astrocyte equations, DOP853, analytic invariants, current-CLI checks."""
from __future__ import annotations
import argparse, concurrent.futures, csv, hashlib, json, math, os, subprocess, sys, time
from pathlib import Path
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
STUDIO=HERE.parents[1]
DEPS=STUDIO.parent/'tmp/python_deps'
if DEPS.exists():sys.path.insert(0,str(DEPS))
import numpy as np
import scipy
from scipy.integrate import solve_ivp
EXE=HERE/'build/Release/astrocyte_audit.exe'
CLI=STUDIO/'build/Release/network_studio_cli.exe'
RESULTS=HERE/'results'
COLS=['GammaA','IP3_uM','Ca_uM','h','xA','GA_uM','GammaS']
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def save(p,r):Path(p).write_text(json.dumps(r,indent=2,ensure_ascii=False,allow_nan=False)+'\n',encoding='utf-8')
def rows(p):
    with Path(p).open(encoding='utf-8',newline='') as f:return list(csv.DictReader(f))
def filename(name,kind):return RESULTS/f'{name}_{kind}'
def flow(y,p,glu):
    ga,I,C,h,x,G,gs=y
    hill=lambda value,K,n:value**n/(value**n+K**n)
    m=(I/(I+p['d1']))*(C/(C+p['d5']))
    q=p['d2']*(I+p['d1'])/(I+p['d3'])
    return np.array([
        p['O_N']*glu*(1-ga)-p['Omega_N']*ga,
        p['O_beta']*ga+p['O_delta']/(1+I/p['kappa_delta'])*hill(C,p['K_delta'],2)
          -p['O_3K']*hill(C,p['K_D'],4)*I/(I+p['K_3K'])-p['Omega_5P']*I,
        (p['Omega_C']*(m*h)**3+p['Omega_L'])*(p['C_T']-(1+p['rho_A'])*C)
          -p['O_P']*hill(C,p['K_P'],2),
        p['O_2']*(q*(1-h)-C*h),
        (1-x)/p['tau_A'], -p['Omega_G']*G,
        p['O_G']*G*(1-gs)-p['Omega_S']*gs])

def run(case,dt,skip):
    name=f'{case}_{dt*1e6:g}'.replace('.','p')
    if not skip:
        start=time.perf_counter();r=subprocess.run([str(EXE),case,str(RESULTS/name),str(dt)],capture_output=True,text=True)
        filename(name,'log.txt').write_text(r.stdout+r.stderr,encoding='utf-8')
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        elapsed=time.perf_counter()-start
    else:elapsed=None
    c=read(filename(name,'experiment.json'));s=read(filename(name,'summary.json'));assert s['complete'] and not s['error']
    ev=rows(filename(name,'events.csv'));data=rows(filename(name,'trace.csv'));t=np.array([float(r['time_s']) for r in data])
    A=np.array([[float(r['A1_'+k]) for k in COLS] for r in data]);p=c['parameters']
    result=dict(name=name,scenario=case,dt_us=dt*1e6,duration_s=c['duration'],wall_s=elapsed,
                astro_events_s=[float(r['time_s']) for r in ev if r['type']=='astro_release'],
                neuron_spikes=s['neuron_spikes'],astro_releases=s['astrocyte_releases'],
                state_min=dict(zip(COLS,A.min(axis=0).tolist())),state_max=dict(zip(COLS,A.max(axis=0).tolist())),
                final_astro=A[-1].tolist(),files_sha256={k:sha(filename(name,k)) for k in ['trace.csv','events.csv','weights.csv','experiment.json','initial_state.csv']})
    assert np.all(A[:,[0,3,4,6]]>=-1e-12) and np.all(A[:,[0,3,4,6]]<=1+1e-12)
    assert np.all(A[:,[1,2,5]]>=-1e-12) and np.max(A[:,2])<=p['C_T']/(1+p['rho_A'])+1e-12
    if case in ['rest','transient','receptors','release']:
        glu=2. if case=='receptors' else 0.
        # The first four variables have no downstream xA/GA/GammaS feedback.
        def rhs_first(time,y):return flow([*y,1,0,0],p,glu)[:4]
        def crossing(time,y):return y[2]-c['astrocytes'][0]['threshold']
        crossing.direction=1
        sol=solve_ivp(rhs_first,(t[0],t[-1]),A[0,:4],method='DOP853',rtol=2e-12,atol=1e-14,
                      t_eval=t,events=crossing,max_step=.02)
        assert sol.success
        errors=np.max(np.abs(A[:,:4]-sol.y.T),axis=0)
        result['dop853_first4_max_abs_errors']=dict(zip(COLS[:4],errors.tolist()))
        result['continuous_upcrossing_times_s']=sol.t_events[0].tolist()
        if case=='release':
            times=result['astro_events_s'];exact=sol.t_events[0]
            assert len(times)==len(exact)==1
            result['release_event_error_s']=float(times[0]-exact[0]);assert 0<=result['release_event_error_s']<=dt*1.001
            tau=p['tau_A'];U=p['U_A'];G0=p['G_release']*U;event=times[0]
            xe=np.where(t<event,1.,1-U*np.exp(-(t-event)/tau))
            ge=np.where(t<event,0.,G0*np.exp(-p['Omega_G']*(t-event)))
            result['event_replay_resource_errors']={'xA':float(np.max(np.abs(A[:,4]-xe))),'GA':float(np.max(np.abs(A[:,5]-ge)))}
            after=t>=event
            ss=solve_ivp(lambda tt,y:[p['O_G']*G0*math.exp(-p['Omega_G']*(tt-event))*(1-y[0])-p['Omega_S']*y[0]],
                         (event,t[-1]),[0.],method='DOP853',t_eval=t[after],rtol=2e-12,atol=1e-14,max_step=.01)
            result['event_replay_resource_errors']['GammaS']=float(np.max(np.abs(A[after,6]-ss.y[0])))
        if case=='receptors':
            k=p['O_N']*glu+p['Omega_N'];eq=p['O_N']*glu/k
            kk=p['O_G']*3+p['Omega_S'];ee=p['O_G']*3/kk
            result['closed_form_receptor_errors']={'GammaA':float(np.max(np.abs(A[:,0]-(eq+(A[0,0]-eq)*np.exp(-k*t))))),
                                                   'GammaS':float(np.max(np.abs(A[:,6]-(ee+(A[0,6]-ee)*np.exp(-kk*t)))))}
    if case=='gap':
        B=np.array([[float(r['A2_'+k]) for k in COLS] for r in data])
        expected_I=.8-.4*np.exp(-1.4*t);expected_C=.2-.1*np.exp(-.8*t)
        result['analytic_diffusion_errors']={'IP3':float(np.max(np.abs(A[:,1]-expected_I))),
                                             'Ca':float(np.max(np.abs(A[:,2]-expected_C))),
                                             'sum_IP3':float(np.max(np.abs(A[:,1]+B[:,1]-1.6))),
                                             'sum_Ca':float(np.max(np.abs(A[:,2]+B[:,2]-.4)))}
    print('CHECKED',name,result.get('astro_events_s'),flush=True)
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('--skip-run',action='store_true');args=p.parse_args();RESULTS.mkdir(exist_ok=True)
    sources=[STUDIO/'network.cpp',STUDIO/'network.h',STUDIO/'io.cpp',STUDIO/'vendor/systems/neuro_astro/model.cuh',
             STUDIO/'vendor/systems/neuro_astro/parameters.inc',STUDIO/'vendor/systems/neuro_astro/variables.inc']
    result=dict(python=sys.version,numpy=np.__version__,scipy=scipy.__version__,scope='Astrocyte mathematical audit; production binaries unchanged',
                cli_sha256=sha(CLI),core_library_sha256=sha(STUDIO/'build/native/network_core.lib'),harness_sha256=sha(EXE),
                source_sha256={str(x.resolve()):sha(x) for x in sources},script_sha256=sha(__file__),runs=[])
    result['audit_sources_sha256']={name:sha(HERE/name) for name in ['harness.cpp','CMakeLists.txt','check.py']}
    result['primary_references']=[
        dict(title='Li and Rinzel 1994',doi='10.1006/jtbi.1994.1041',url='https://pubmed.ncbi.nlm.nih.gov/8176949/',scope='Origin of the reduced calcium/IP3-receptor gating model'),
        dict(title='De Pitta et al. 2009',url='https://arxiv.org/pdf/0912.3057',sections='equations 5-6, 14-17 and Table 1',scope='IP3 metabolism and Li-Rinzel core; original G-ChI uses a different algebraic glutamate/Ca-dependent receptor term'),
        dict(title='De Pitta and Brunel 2016',doi='10.1155/2016/7607924',url='https://webhome.phy.duke.edu/~nb170/pdfs/depitta16.pdf',pages=[17,18],equations='A.3-A.11',scope='Closest receptor/IP3/Ca/resource/feedback structure; current SIC remains a further reduction')]
    if not args.skip_run:subprocess.run([str(EXE),'probe',str(RESULTS)],check=True)
    cases=['rest','transient','release','gap','receptors','active','active_off']
    old={r['name']:r['wall_s'] for r in read(HERE/'findings.json')['runs']} if (HERE/'findings.json').exists() else {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures=[pool.submit(run,c,d,args.skip_run) for c in cases for d in [.000025,.0000125,.00000625,.000003125]]
        futures.append(pool.submit(run,'initial_above',.000025,args.skip_run))
        for f in concurrent.futures.as_completed(futures):
            r=f.result()
            if r['wall_s'] is None:r['wall_s']=old.get(r['name'])
            result['runs'].append(r)
    params=read(filename('rest_25','experiment.json'))['parameters']
    probes=rows(RESULTS/'rhs_probes.csv');maxerr=np.zeros(7);maxkernel=np.zeros(7);boundary=[]
    for row in probes:
        y=[float(row[k]) for k in ['GammaA','IP3','Ca','h','xA','GA','GammaS']];g=float(row['glutamate'])
        core=np.array([float(row[f'network_{i}']) for i in range(7)]);kernel=np.array([float(row[f'kernel_{i}']) for i in range(7)])
        maxerr=np.maximum(maxerr,np.abs(core-flow(y,params,g)));maxkernel=np.maximum(maxkernel,np.abs(core-kernel))
        for k in [0,1,2,3,4,5,6]:
            if y[k]==0:boundary.append(core[k]>=-1e-13)
            upper=params['C_T']/(1+params['rho_A']) if k==2 else 1.
            if k in [0,2,3,4,6] and y[k]==upper:boundary.append(core[k]<=1e-13)
    assert all(boundary) and max(maxerr)<1e-12 and max(maxkernel)<1e-12
    result['rhs_probes']=dict(count=len(probes),independent_formula_max_abs_error=dict(zip(COLS,maxerr.tolist())),
                             original_kernel_max_abs_error=dict(zip(COLS,maxkernel.tolist())),boundary_checks=len(boundary),boundaries_inward=all(boundary))
    sic=rows(RESULTS/'feedback_probes.csv');delta=float(sic[1]['post_dV_dt'])-float(sic[0]['post_dV_dt'])
    result['SIC_probe']=dict(expected_added_dV_dt=1000*.02*2/3*65,actual_added_dV_dt=delta,
                             duplicate_contact_difference=float(sic[2]['post_dV_dt'])-float(sic[1]['post_dV_dt']))
    assert abs(delta-result['SIC_probe']['expected_added_dV_dt'])<1e-10 and result['SIC_probe']['duplicate_contact_difference']==0
    result['gap_physical_interpretation']=rows(RESULTS/'gap_balance.csv')
    result['convergence']={}
    for case in cases:
        rr=sorted([r for r in result['runs'] if r['scenario']==case],key=lambda r:r['dt_us'],reverse=True)
        finest=rr[-1]
        result['convergence'][case]=[dict(dt_us=r['dt_us'],max_final_astro_difference_vs_3p125us=float(np.max(np.abs(np.array(r['final_astro'])-finest['final_astro']))),
                                        final_difference_by_variable=dict(zip(COLS,np.abs(np.array(r['final_astro'])-finest['final_astro']).tolist())),
                                        astro_releases=r['astro_releases'],neuron_spikes=r['neuron_spikes']) for r in rr]
    result['cli_harness_checks']={}
    for case in ['active','active_off']:
        name=case+'_25';dest='cli_'+name
        if not args.skip_run:
            proc=subprocess.run([str(CLI),'--config',str(filename(name,'experiment.json')),'--output',str(RESULTS/dest)],capture_output=True,text=True)
            filename(dest,'log.txt').write_text(proc.stdout+proc.stderr,encoding='utf-8');assert proc.returncode==0
        checks={k:sha(filename(name,k+'.csv'))==sha(filename(dest,k+'.csv')) for k in ['trace','events','weights']}
        assert all(checks.values());result['cli_harness_checks'][case]=checks
    for r in result['runs']:
        if 'dop853_first4_max_abs_errors' in r:assert max(r['dop853_first4_max_abs_errors'].values())<1e-9
        if 'event_replay_resource_errors' in r:assert max(r['event_replay_resource_errors'].values())<1e-9
        if 'closed_form_receptor_errors' in r:assert max(r['closed_form_receptor_errors'].values())<1e-9
        if 'analytic_diffusion_errors' in r:assert max(r['analytic_diffusion_errors'].values())<1e-9
    assert all(sha(k)==v for k,v in result['source_sha256'].items()) and sha(CLI)==result['cli_sha256']
    result['status']='PASS: equations/numerics within tested regimes; physical calcium-gap limitation documented separately'
    result['independence_scope']={
        'RHS':'Independent Python equations versus both network_core.lib and header model.cuh; 200 states and 34 boundary conditions.',
        'DOP853':'Isolated first-four-variable astrocyte module with prescribed zero or constant glutamate, not an independent full neuron-synapse-astrocyte trajectory.',
        'resources':'Analytic xA/GA and independent scalar GammaS ODE conditional on exported release events; separate exact continuous calcium event-time check.',
        'full_system':'Step refinement of the complete 2-pre/1-post/1-astrocyte system on 5 seconds; binary-equivalence check against the existing production CLI; not an independent full-system ODE reference.'}
    result['confirmed_astro_implementation_defects_in_tested_default_regimes']=[]
    result['limitations']=[dict(id='ASTRO-L1',severity='P2 scientific interpretation when calcium gap is used as physical intercellular transport',
        location='network.cpp:276,284-287',condition='Gap.calcium > 0; fixed local C_T closure',
        finding='Cytosolic concentrations exchange conservatively, but each inferred local total remains constant. With all ER fluxes off, an inferred ER concentration changes to compensate calcium-gap flux.',
        reproduction='harness probe / gap scenarios: dC=(+.08,-.08) uM/s but inferred dCER=(-.432432,+.432432) uM/s while dCT=0.',
        recommendation='Call this phenomenological calcium coupling, or add per-cell total calcium / explicit ER state if physical mass transport is intended.',
        boundary='Default calcium gap is zero; ordinary IP3-only coupling and isolated astrocyte equations are unaffected.'),
        dict(id='ASTRO-L2',severity='scope',location='network.cpp:281-282; model.cuh:85-87',finding='SIC is an instantaneous saturating glutamate-dependent conductance without explicit NMDAR kinetics or magnesium block; README already discloses this reduction.'),
        dict(id='ASTRO-L3',severity='numerical scope',location='network.cpp:105-113,333-341',finding='Wide globally accepted rate ranges are not an RK4 stability guarantee; the root audit independently tests that problem. This component audit validates the stated default/controlled parameter regimes only.')]
    result['runs'].sort(key=lambda r:(r['scenario'],-r['dt_us']))
    save(HERE/'findings.json',result)
    print('ALL CHECKS PASS',flush=True)

if __name__=='__main__':main()
