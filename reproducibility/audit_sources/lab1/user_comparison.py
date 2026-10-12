"""Additional Lab 1 protocols named in the user's actual NeuroNet report.

Produces diagnostics, never runs or overwrites original NeuroNet or old results.
"""
from pathlib import Path
import copy
import math
import sys
import numpy as np
from PIL import Image
import audit
from reference_tm import tm_responses, euler_tm_responses

OUT=audit.OUT
USER=Path('E:/asp/26_27/labs/lab1_report')


def translated_euler(rate, onset_ms, dt_ms=.5, duration_ms=5000.):
    """Generalize lab1/neuronet_probe.py without claiming original execution."""
    v,u,t=-70.,0.,0.
    flag,timer=False,0.
    thresholds,registered=[],[]
    period=1000/rate
    for _ in range(round(duration_ms/dt_ms)):
        phase=(int(t*10) % int(period*10))/10
        current=30. if onset_ms<phase<=onset_ms+3 else 0.
        dv=.04*v*v+5*v+140-u+current
        du=.02*(.2*v-u)
        v+=dt_ms*dv;u+=dt_ms*du
        if v>=30:
            thresholds.append(t/1000);flag=True;v=-65.;u+=8.
        if timer!=0:
            timer-=dt_ms
            if timer<=0:
                flag,timer=False,0.
        if timer==0 and flag:
            registered.append(t/1000);timer=1.
        t+=dt_ms
    assert thresholds==registered
    return registered


def period_counts(times,rate,duration=5.,discard=1.):
    counts=[sum(a<=t<a+1/rate-1e-10 for t in times) for a in np.arange(discard,duration,1/rate)]
    return {str(n):counts.count(n) for n in sorted(set(counts))}


def screenshot_period(name,rect):
    """Approximate blue Em peak columns, axes chosen by visual inspection.

    Full axis is 5000ms. Pixel quantization is reported, not ignored.
    """
    x0,y0,x1,y1=rect
    rgb=np.asarray(Image.open(USER/'images'/name).convert('RGB'))
    block=rgb[y0:y1+1,x0:x1+1]
    mask=(block[:,:,2]>180)&(block[:,:,0]<70)&(block[:,:,1]<100)
    cols=np.flatnonzero(mask.sum(axis=0)>120)
    groups=[]
    for c in cols:
        if not groups or c>groups[-1][-1]+1:groups.append([int(c)])
        else:groups[-1].append(int(c))
    peaks=[x0+float(np.mean(g)) for g in groups]
    ms_per_pixel=5000/(x1-x0)
    intervals=np.diff(peaks)*ms_per_pixel
    return dict(image=name,axis_rectangle_pixels=rect,axis_duration_ms=5000,
                peak_columns=peaks,detected_peaks=len(peaks),ms_per_pixel=ms_per_pixel,
                approximate_mean_isi_ms=float(intervals.mean()),
                interval_range_ms=[float(intervals.min()),float(intervals.max())],
                limitation='Approximate raster-image measurement; no original event log, initial state or integration step.')


def main():
    audit.configure()
    cli_hash=audit.digest(audit.CLI)
    report=dict(cli_sha256=cli_hash,
        source_report=str(USER/'Отчёт_ЛР1.md'),
        source_sha256={str(p.relative_to(USER)):audit.digest(p) for p in USER.rglob('*') if p.is_file()},
        source_observations=[
            'Task1 is claimed by the experimenter, but no separate Fire! screenshot was supplied.',
            'Task2: b=.27, a=.02,c=-65,d=8, individual sigma0. External stimulation is not separately shown as disabled.',
            'Task3A 2Hz table: (50,0),(53,30),(500,0);20Hz:(20,0),(23,30),(50,0). Step and global Noise are not visible.',
            'Task3A shows Output envelopes, not zoomed Em: close spikes cannot be counted from these images.',
            'Task3B: constant3.8, individual sigma0; stable repetitive Em.',
            'Task4 report gives sigma0,4,4.5; individual4.5 visible; global sigma value is outside the supplied global-noise image.',
            'Neither stimulus/initial-state/time-series/seed completeness nor exact numerical equivalence follows from the screenshots.'
        ], noise={},pulse_protocols={})
    report['image_periods']={
        'autonomous_b027':screenshot_period('task2.png',(398,116,1009,608)),
        'dc3p8':screenshot_period('task3b.png',(429,121,1042,613)),
    }
    print('Raster periods',report['image_periods'],flush=True)
    for sigma in [0.,4.,4.5]:
        seeds=[42] if sigma==0 else [11,23,42,71,101]
        rows=[]
        for seed in seeds:
            c=audit.read(audit.LAB/'configs/07_noise_individual.json')
            c.update(name=f'User Lab1 noise sigma{sigma:g} seed{seed}',duration=10.,sample=.0002,noise_sigma=sigma,seed=seed)
            for n in c['neurons']:n.update(individual_noise=False,noise_sigma=0.,amplitude=0.,current=0.)
            name=f'user_noise_global_{str(sigma).replace(".","p")}_seed{seed}'
            p=audit.run_extra(name,c)
            stats=[audit.lab.stats(np.array(audit.events(p,i)),10.,1.) for i in [1,2]]
            rows.append(dict(seed=seed,neurons=stats))
            if seed==42:
                individual=copy.deepcopy(c)
                individual['name']+= ' / explicit individual'
                individual['noise_sigma']=8.
                for n in individual['neurons']:n.update(individual_noise=True,noise_sigma=sigma)
                q=audit.run_extra(name+'_individual',individual)
                exact={suffix:audit.digest(Path(str(p)+'_'+suffix))==audit.digest(Path(str(q)+'_'+suffix)) for suffix in ['trace.csv','events.csv']}
                assert all(exact.values())
        rates=[n['rate_hz'] for row in rows for n in row['neurons']]
        report['noise'][str(sigma)]=dict(noise_hold_ms=1.,simulation_duration_s=10.,window_s=[1,10],
             runs=rows,mean_rate_hz=float(np.mean(rates)),sd_across_neuron_seed_hz=float(np.std(rates,ddof=1)),
             same_seed_explicit_vs_global_csv_identical=exact)
        print('User noise',sigma,report['noise'][str(sigma)]['mean_rate_hz'],flush=True)
    for hz,onset_ms in [(2,50),(20,20)]:
        c=audit.read(audit.LAB/'configs/02_single_pulse.json')
        c.update(name=f'User Lab1 screenshot protocol {hz}Hz 30x3ms',duration=5.,sample=.0002)
        c['neurons'][0].update(voltage=-70.,izh_u=0.,equilibrium_u=False,frequency=float(hz),
                               start=onset_ms/1000,stop=5.,width=.003)
        p=audit.run_extra(f'user_pulse_{hz}hz',c)
        ev=audit.events(p)
        ref=audit.adaptive(c)
        audit.write(OUT/'reference'/f'user_pulse_{hz}hz.json',ref)
        translated=translated_euler(hz,onset_ms)
        peaks_native=tm_responses(ev)
        peaks_translated=euler_tm_responses(translated,dt=.0005)
        item=dict(stimulus_hz=hz,onset_ms=onset_ms,duration_s=5.,analysis_window_s=[1,5],
            native_vs_DOP853=audit.comparison(ev,ref['events_s']),
            native_events_per_period=period_counts(ev,hz),native_late_spikes=sum(1<=t<5 for t in ev),
            native_last_output_peak=peaks_native[-1]['peak'],
            translated_Euler_dt_ms=.5,translated_events_s=translated,
            translated_events_per_period=period_counts(translated,hz),translated_late_spikes=sum(1<=t<5 for t in translated),
            translated_last_output_peak=peaks_translated[-1]['peak'],
            caveat='Diagnostic translation, not original binary execution. Noise0, V0=-70,u0=0 and Euler0.5ms are chosen conditions; screenshot does not confirm all of these.')
        assert item['native_vs_DOP853']['same_spike_count']
        if hz==20:
            for dt in [.0000125,.000005]:
                finer=copy.deepcopy(c);finer['dt']=dt
                q=audit.run_extra(f'user_pulse_20hz_dt{round(dt*1e7)}',finer)
                item[f'native_dt{dt:g}_vs_DOP853']=audit.comparison(audit.events(q),ref['events_s'])
        report['pulse_protocols'][str(hz)]=item
        print('User pulse',hz,item,flush=True)
    c=audit.read(audit.LAB/'configs/sweep_dc_3p80.json')
    ref=audit.adaptive(c)
    audit.write(OUT/'reference/user_dc3p8.json',ref)
    fine=copy.deepcopy(c);fine['dt']=.0000125
    q=audit.run_extra('user_dc3p8_12p5us',fine)
    report['dc3p8']=dict(native25us=audit.comparison(audit.events(audit.lab.RAW/'sweep_dc_3p80'),ref['events_s']),
        native12_5us=audit.comparison(audit.events(q),ref['events_s']),
        native_metrics=audit.lab.stats(np.array(audit.events(audit.lab.RAW/'sweep_dc_3p80')),5.,1.))
    # Linearization of the continuous subthreshold equations is independent of
    # reset-driven firing. It does not alone establish a global firing threshold.
    a,b=.02,.2
    v=(a-5)/.08
    loss_current=b*v-(.04*v*v+5*v+140)
    report['equilibrium_analysis']=dict(RS_stable_rest_mV=-70.,RS_rest_u=-14.,
        RS_Jacobian_eigenvalues_per_ms=[dict(real=float(x.real),imag=float(x.imag)) for x in np.linalg.eigvals([[-.6,-1],[.004,-.02]])],
        autonomous_b027_equilibrium_discriminant=(5-.27)**2-4*.04*140,
        local_equilibrium_stability_loss_current=loss_current,
        caveat='Local continuous-ODE stability boundary, not a proof of a global/reset-dependent firing threshold.')
    assert cli_hash==audit.digest(audit.CLI)
    audit.write(OUT/'user_comparison.json',report)


if __name__=='__main__':main()
