"""One unretuned N=11 control after discovering 11 nodes in the original image."""
import copy, csv, math, statistics, subprocess, sys, time
sys.dont_write_bytecode=True
from details import HERE, STUDIO, load, save, sha

out=HERE/'ring11';out.mkdir(exist_ok=True)
base=STUDIO/'lab2/configs/rings/tm_n10_w1.json'
c=load(base);c['name']='Lab2 audit N11 w1 (screenshot node count control)'
c['neurons'].append(copy.deepcopy(c['neurons'][-1]))
for i,n in enumerate(c['neurons']):
    angle=2*math.pi*i/11
    n.update(name=f'N{i+1}',px=.5+.35*math.cos(angle),py=.5+.35*math.sin(angle))
c['synapses'].append(copy.deepcopy(c['synapses'][-1]))
for i,s in enumerate(c['synapses']):s.update(source=i,target=(i+1)%11)
save(out/'config.json',c)
exe=STUDIO/'build/Release/network_studio_cli.exe'
command=[str(exe),'--config',str(out/'config.json'),'--output',str(out/'tm_n11_w1')]
start=time.perf_counter()
run=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'run.log').write_text(run.stdout+run.stderr,encoding='utf-8')
assert run.returncode==0,run.stdout+run.stderr
summary=load(out/'tm_n11_w1_summary.json')
assert summary['complete'] and not summary['error'] and abs(summary['time_s']-5)<1e-12
counts=[0]*11;late=[0]*11;events=[]
with (out/'tm_n11_w1_events.csv').open(newline='') as f:
    for r in csv.DictReader(f):
        t=float(r['time_s']);i=int(r['node_index_1based'])-1
        assert r['type']=='spike' and math.isfinite(t) and 0<t<=5 and 0<=i<11
        counts[i]+=1;late[i]+=4<=t<=5;events.append((t,i+1))
assert counts==summary['neuron_spikes']
with (out/'tm_n11_w1_trace.csv').open(newline='') as f:
    rows=list(csv.DictReader(f))
assert all(math.isfinite(float(v)) for row in rows for v in row.values())
assert len(rows)==5001 and float(rows[-1]['time_s'])==5
def activity(events):
    late_events=[(t,i) for t,i in events if t>=4]
    fraction=sum(b[1]==a[1]%11+1 for a,b in zip(late_events,late_events[1:]))/(len(late_events)-1)
    n1=[t for t,i in late_events if i==1]
    isi=[b-a for a,b in zip(n1,n1[1:])]
    return dict(late_ordered_transition_fraction=fraction,n1_late_rate_hz=len(n1),
        n1_late_mean_isi_ms=1000*statistics.mean(isi),n1_late_isi_rate_hz=1/statistics.mean(isi),
        n1_late_isi_cv=statistics.pstdev(isi)/statistics.mean(isi))
result=dict(passed=True,reason='Original ring10_w1.png contains11nodes; do not assume filename means N10.',
    sole_dynamical_change_from_saved_N10='One additional identical RS neuron and one TM edge; relabel circular layout only.',
    neurons=11,weight=1,duration_s=5,dt_us=25,counts=counts,late_4_to_5s_counts=late,
    events=events,last_spike_s=events[-1][0],sustained_circulation_to_5s=all(x>=2 for x in late) and events[-1][0]>=4.9,
    source_config_sha256=sha(base),config_sha256=sha(out/'config.json'),binary_sha256=sha(exe),
    command=command,wall_seconds=time.perf_counter()-start,
    output_sha256={p.name:sha(p) for p in out.glob('tm_n11_w1*')},**activity(events))
result['sustained_circulation_to_5s'] &= result['late_ordered_transition_fraction']>=.95
save(out/'findings.json',result)
print(result['counts'],'last_spike_s',result['last_spike_s'],'sustained',result['sustained_circulation_to_5s'],flush=True)

# A separate half-step run checks the newly observed circulating regime.
c['dt']/=2;save(out/'config_fine.json',c)
fine_command=[str(exe),'--config',str(out/'config_fine.json'),'--output',str(out/'fine')]
start=time.perf_counter()
run=subprocess.run(fine_command,capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'fine.log').write_text(run.stdout+run.stderr,encoding='utf-8')
assert run.returncode==0,run.stdout+run.stderr
fine_summary=load(out/'fine_summary.json')
assert fine_summary['complete'] and not fine_summary['error'] and abs(fine_summary['time_s']-5)<1e-12
with (out/'fine_events.csv').open(newline='') as f:
    fine_events=[(float(r['time_s']),int(r['node_index_1based'])) for r in csv.DictReader(f)]
fine_counts=[sum(i==n for t,i in fine_events) for n in range(1,12)]
fine_late=[sum(i==n and t>=4 for t,i in fine_events) for n in range(1,12)]
assert fine_counts==fine_summary['neuron_spikes'] and [i for t,i in fine_events]==[i for t,i in events]
with (out/'fine_trace.csv').open(newline='') as f:
    fine_rows=list(csv.DictReader(f))
assert len(fine_rows)==5001 and all(math.isfinite(float(v)) for row in fine_rows for v in row.values())
fine=dict(passed=True,neurons=11,weight=1,duration_s=5,dt_us=12.5,counts=fine_counts,
    late_4_to_5s_counts=fine_late,last_spike_s=fine_events[-1][0],**activity(fine_events),
    counts_equal=counts==fine_counts,
    maximum_ordinal_event_difference_ms=max(abs(a[0]-b[0])*1000 for a,b in zip(events,fine_events)),
    config_sha256=sha(out/'config_fine.json'),binary_sha256=sha(exe),command=fine_command,
    wall_seconds=time.perf_counter()-start,output_sha256={p.name:sha(p) for p in out.glob('fine_*') if p.name!='fine_findings.json'})
fine['sustained_circulation_to_5s']=all(x>=2 for x in fine_late) and fine['last_spike_s']>=4.9 and fine['late_ordered_transition_fraction']>=.95
save(out/'fine_findings.json',fine)
print('N11 half-step:',fine,flush=True)
