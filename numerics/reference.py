"""Independent adaptive hybrid ODE reference (SciPy DOP853 / Radau).

Uses full continuous equations, exact input breakpoints, terminal upward AND
downward threshold events, instantaneous resets, and direct sums over all past
spikes for learning. No production integrator, RK4 recurrence or dense output
is used. Time is in seconds throughout.
"""
import math
import numpy as np
from scipy.integrate import solve_ivp


def hh_rates(v):
    w = -(v+65)
    def q(z):
        return 1. if abs(z)<1e-14 else z/math.expm1(z)
    return np.array([q((w+25)/10), .07*math.exp(w/20), .1*q((w+10)/10)]), np.array([
        4*math.exp(w/18), 1/(math.exp((w+30)/10)+1), .125*math.exp(w/80)])


def initial(c):
    nn, ns, na = len(c['neurons']), len(c['synapses']), len(c['astrocytes'])
    y = np.zeros(4*nn+4*ns+7*na)
    for i,n in enumerate(c['neurons']):
        y[4*i] = n['voltage']
        if n['model']=='izhikevich':
            y[4*i+1] = n['izh_b']*n['voltage'] if n['equilibrium_u'] else n['izh_u']
        else:
            a,b = hh_rates(n['voltage']); y[4*i+1:4*i+4] = a/(a+b)
    for j in range(ns): y[4*nn+4*j+1] = 1
    for j,a in enumerate(c['astrocytes']):
        b=4*(nn+ns)+7*j; y[b:b+7]=[0,a['ip3'],a['calcium'],a['h'],1,0,0]
    return y


def input_intervals(c):
    intervals=[];boundaries={0.,c['duration']}
    for n in c['neurons']:
        spans=[]
        if n['frequency']>0 and n['amplitude']!=0:
            period=1/n['frequency']
            relative=n.get('pulse_intervals') or [dict(start=0.,stop=min(n['width'],period))]
            for k in range(max(0,math.ceil((min(n['stop'],c['duration'])-n['start'])/period))):
                for r in relative:
                    left=n['start']+k*period+r['start']
                    right=n['start']+(k+1)*period if r['stop']==period else n['start']+k*period+r['stop']
                    left=max(0.,left);right=min(right,n['stop'],c['duration'])
                    if left<right:
                        spans.append((left,right));boundaries.update((left,right))
        intervals.append(spans)
    assert c['noise_sigma']==0 and not any(n['noise_sigma'] for n in c['neurons']), 'Reference fixtures are deterministic'
    return intervals,sorted(boundaries)


def learn(c,weights,past,t,pre_group):
    """Enumerate strictly older events; simultaneous events enter history last."""
    out=weights.copy()
    for j,s in enumerate(c['synapses']):
        if not s['enabled'] or not s['stdp']: continue
        pre=s['source'] in pre_group;post=s['target'] in pre_group
        a=math.fsum(math.exp(-(t-u)/s['tau_plus']) for u in past[s['source']])
        b=math.fsum(math.exp(-(t-u)/s['tau_minus']) for u in past[s['target']])
        slow=math.fsum(math.exp(-(t-u)/s['tau_slow']) for u in past[s['target']])
        multiplicative=s['stdp_rule']!='additive'
        up=s['weight_max']-weights[j] if multiplicative else 1.
        down=weights[j]-s['weight_min'] if multiplicative else 1.
        plus=s['a_plus']*up*a*(slow if s['stdp_rule']=='triplet' else 1.) if post else 0.
        minus=s['a_minus']*down*b if pre else 0.
        out[j]=max(s['weight_min'],min(s['weight_max'],weights[j]+plus-minus))
    return out


def rhs(c,y,current,weights):
    p=c['parameters'];nn=len(c['neurons']);ns=len(c['synapses']);na=len(c['astrocytes'])
    d=np.zeros_like(y);drive=current.copy();glut=np.zeros(na);contacts=[set() for _ in range(na)]
    for j,s in enumerate(c['synapses']):
        b=4*(nn+j);u,x,active,transmitter=y[b:b+4]
        if s['model']!='exponential_current':
            d[b]=-u/s['tau_f'];d[b+1]=(1-x-(active if s['model']=='tsodyks_markram' else 0))/s['tau_d']
        d[b+2]=-active/s['tau_syn'];d[b+3]=-p['Omega_Y']*transmitter
        if not s['enabled']:continue
        drive[s['target']]+=weights[j]*active*(s['conductance']*(s['reversal']-y[4*s['target']]) if s['model']=='conductance_stp' else s['gain'])
        if s['astro']>=0:
            glut[s['astro']]+=s['spill']*transmitter;contacts[s['astro']].add(s['target'])
    for j,a in enumerate(c['astrocytes']):
        b=4*(nn+ns)+7*j;receptor,ip3,ca,h,x,ga,feedback=y[b:b+7]
        hill=lambda v,k,power: v**power/(v**power+k**power)
        d[b]=p['O_N']*glut[j]*(1-receptor)-p['Omega_N']*receptor
        d[b+1]=p['O_beta']*receptor+p['O_delta']*p['kappa_delta']/(p['kappa_delta']+ip3)*hill(ca,p['K_delta'],2)-p['O_3K']*hill(ca,p['K_D'],4)*ip3/(ip3+p['K_3K'])-p['Omega_5P']*ip3
        opening=ip3/(ip3+p['d1'])*ca/(ca+p['d5'])
        d[b+2]=(p['Omega_C']*opening**3*h**3+p['Omega_L'])*(p['C_T']-(1+p['rho_A'])*ca)-p['O_P']*hill(ca,p['K_P'],2)
        q=p['d2']*(ip3+p['d1'])/(ip3+p['d3'])
        d[b+3]=p['O_2']*(q*(1-h)-ca*h);d[b+4]=(1-x)/p['tau_A'];d[b+5]=-p['Omega_G']*ga
        d[b+6]=p['O_G']*ga*(1-feedback)-p['Omega_S']*feedback
        if a['feedback']:
            for i in contacts[j]:drive[i]+=a['sic']*ga/(p['K_SIC']+ga)*(p['E_syn']-y[4*i])
    for g in c['gaps']:
        a=4*(nn+ns)+7*g['a'];b=4*(nn+ns)+7*g['b']
        for offset,key in ((1,'ip3'),(2,'calcium')):
            flux=g[key]*(y[b+offset]-y[a+offset]);d[a+offset]+=flux;d[b+offset]-=flux
    for i,n in enumerate(c['neurons']):
        b=4*i;v=y[b]
        if n['model']=='izhikevich':
            d[b]=1000*(.04*v*v+5*v+140-y[b+1]+drive[i]);d[b+1]=1000*n['izh_a']*(n['izh_b']*v-y[b+1])
        else:
            m,h,g=y[b+1:b+4];a,beta=hh_rates(v)
            ionic=p['g_Na']*m**3*h*(v-p['E_Na'])+p['g_K']*g**4*(v-p['E_K'])+p['g_L']*(v-p['E_L'])
            d[b]=1000*(drive[i]-ionic)/p['C_m'];d[b+1:b+4]=1000*(a*(1-y[b+1:b+4])-beta*y[b+1:b+4])
    return d


def solve(c,method='DOP853'):
    nn,ns=len(c['neurons']),len(c['synapses']);y=initial(c)
    weights=np.array([s['weight'] for s in c['synapses']],dtype=float);past=[[] for _ in c['neurons']]
    levels=[(4*i,n['izh_peak'] if n['model']=='izhikevich' else c['parameters']['V_spike']) for i,n in enumerate(c['neurons'])]
    levels += [(4*(nn+ns)+7*i+2,a['threshold']) for i,a in enumerate(c['astrocytes'])]
    armed=[y[b]<v for b,v in levels];events=[];segments=[];weight_segments=[]
    spans,boundaries=input_intervals(c)
    for left,right in zip(boundaries[:-1],boundaries[1:]):
        mid=(left+right)/2
        current=np.array([n['current']+(n['amplitude'] if any(a<=mid<b for a,b in spans[i]) else 0.) for i,n in enumerate(c['neurons'])])
        cursor=left
        while cursor<right:
            functions=[]
            for i,(coordinate,peak) in enumerate(levels):
                def event(t,state,b=coordinate,v=peak):return state[b]-v
                event.terminal=True;event.direction=1 if armed[i] else -1;functions.append(event)
            sol=solve_ivp(lambda t,state:rhs(c,state,current,weights),(cursor,right),y,method=method,
                rtol=2e-11,atol=2e-13,max_step=50e-6,dense_output=True,events=functions)
            if not sol.success:raise RuntimeError(sol.message)
            end=float(sol.t[-1]);segments.append((cursor,end,sol.sol));weight_segments.append((cursor,end,weights.copy()))
            y=sol.y[:,-1].copy()
            hits=[i for i,times in enumerate(sol.t_events) if len(times)]
            if not hits:cursor=right;break
            velocity=rhs(c,y,current,weights)
            # solve_ivp may report only the first terminal root for an exact tie.
            for i,(b,v) in enumerate(levels):
                if i not in hits and abs(y[b]-v)<2e-8 and (velocity[b]>0)==armed[i]:hits.append(i)
            up=sorted(i for i in hits if armed[i]);fired={i for i in up if i<nn}
            weights=learn(c,weights,past,end,fired)
            for i in fired:
                n=c['neurons'][i]
                if n['model']=='izhikevich':y[4*i]=n['izh_c'];y[4*i+1]+=n['izh_d']
                for j,s in enumerate(c['synapses']):
                    if not s['enabled'] or s['source']!=i:continue
                    b=4*(nn+j);release=1.;prob=s['release']
                    if s['astro']>=0 and c['astrocytes'][s['astro']]['feedback']:
                        a=c['astrocytes'][s['astro']];prob+=(a['alpha']-prob)*y[4*(nn+ns)+7*s['astro']+6]
                    if s['model']!='exponential_current':y[b]+=prob*(1-y[b]);release=y[b]*y[b+1];y[b+1]-=release
                    y[b+2]+=release
                    excitatory=s['reversal']>-40 if s['model']=='conductance_stp' else (s['stdp'] or s['weight']>0)
                    if excitatory:y[b+3]+=c['parameters']['Y_release']*release
            for i in up:
                if i>=nn:
                    b=4*(nn+ns)+7*(i-nn);release=c['parameters']['U_A']*y[b+4];y[b+4]-=release;y[b+5]+=c['parameters']['G_release']*release
                events.append((end,i if i<nn else i-nn,i>=nn))
            for i in fired:past[i].append(end)
            for i in hits:
                if i<nn and c['neurons'][i]['model']=='izhikevich':armed[i]=True
                else:armed[i]=not armed[i]
            if end<=cursor:raise RuntimeError('Reference event did not advance time')
            cursor=end
    def sample(times):
        times=np.asarray(times);out=np.full((len(y),len(times)),np.nan);wout=np.zeros((ns,len(times)))
        for (left,right,dense),(_,_,w) in zip(segments,weight_segments):
            mask=(times>=left)&(times<=right)
            if mask.any():
                out[:,mask]=dense(times[mask]);wout[:,mask]=w[:,None]
        final=times==c['duration'];out[:,final]=y[:,None];wout[:,final]=weights[:,None]
        assert np.isfinite(out).all()
        return out,wout
    return dict(events=events,final=y,weights=weights,sample=sample,method=method)
