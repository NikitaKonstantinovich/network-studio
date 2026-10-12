"""Additional initial/reset/detector checks on the unchanged production CLI."""
import copy
import json
import subprocess

from audit import CLI, HERE, EXPECTED_CLI_SHA, native, sha, write_json


def main():
    assert sha(CLI) == EXPECTED_CLI_SHA
    base=json.loads((HERE/'base.json').read_text(encoding='utf-8'))
    base['duration']=.05;base['sample']=.00005
    base['neurons'][0].update(current=10,amplitude=0)
    observations=[]
    traces=[]
    for threshold in (0,20,100):
        c=copy.deepcopy(base);c['parameters']['V_spike']=threshold
        times,states,events,summary=native(c,'hh_detector_'+str(threshold))
        traces.append(states)
        observations.append({'V_spike_mV':threshold,'events':len(events),'first_event_ms':events[0] if events else None,
                             'peak_voltage_mV':float(states[0].max())})
    import numpy as np
    same=all(np.array_equal(traces[0],x) for x in traces[1:])
    assert same and observations[0]['events']>0 and observations[-1]['events']==0
    rejected=[]
    for name,fields in [('initial_at_peak',{'voltage':30}),('initial_above_peak',{'voltage':31}),
                        ('reset_at_peak',{'voltage':-65,'izh_peak':0,'izh_c':0})]:
        c=copy.deepcopy(base);c['neurons'][0].update(model='izhikevich',**fields)
        path=HERE/'runs'/('invalid_'+name+'.json');write_json(path,c)
        result=subprocess.run([str(CLI),'--config',str(path),'--output',str(HERE/'runs'/('invalid_'+name))],capture_output=True,text=True)
        rejected.append({'name':name,'exit_code':result.returncode,'message':result.stderr.strip()})
    assert all(x['exit_code']!=0 and 'below the spike peak' in x['message'] for x in rejected)
    write_json(HERE/'boundary_findings.json',{'cli_sha256':sha(CLI),'HH_threshold_is_observer_only':same,
        'HH_detector_variants':observations,'Izh_invalid_initial_reset_rejected':rejected,
        'successful_native_runs':3,'rejected_native_configs':3})
    print(json.dumps({'HH_state_identical':same,'HH_detector_events':[x['events'] for x in observations],
                      'invalid_Izh_rejected':len(rejected)}))


if __name__=='__main__':main()
