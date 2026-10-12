"""Assemble review classification from completed numerical outputs; no simulation."""
import json
from pathlib import Path
from audit import HERE, CLI, EXPECTED_CLI_SHA, sha, write_json

data=json.loads((HERE/'findings.json').read_text(encoding='utf-8'))
boundary=json.loads((HERE/'boundary_findings.json').read_text(encoding='utf-8'))
assert sha(CLI)==EXPECTED_CLI_SHA
assert all(r['counts_equal'] for r in data['checks'])
assert all(r['gates_in_unit_interval'] for r in data['checks'] if r['name'].startswith('hh'))
assert all(r['counts_equal'] and r['max_event_error_ms']<1e-7 for r in data['Radau_crosschecks'])
assert data['template_core_bridge']['bridge_max_neuron_state_difference']==0
issues=[
    {'id':'NEURON-N1','priority':'P2 numerical-accuracy limitation, not incorrect RHS',
     'location':'standalone/network_studio/network.cpp:333-349,404; systems/neuro_astro/model.cuh:138-140',
     'claim':'End-of-step threshold/reset is not event-located. The bound dt describes local placement, not error versus the exact hybrid trajectory.',
     'evidence':'Izh FS, I=10, a=.1,b=.2,c=-65,d=2, 200ms: 28 spikes for all references/runs; max time error .863387/.650887/.203387ms at25/12.5/5us.',
     'impact':'Spike/reset delays alter subsequent recovery and accumulate; RK4 smooth-flow order does not imply fourth-order spike times.',
     'recommendation':'Qualify timing claims and use convergence checks; add event location plus split-step reset if precise spike timing is required.'},
    {'id':'NEURON-N2','priority':'P3 initial-condition scope limitation, documented behavior',
     'location':'standalone/network_studio/network.cpp:188-199; network.h:32-43',
     'claim':'HH gates are always initialized to equilibrium at the configured V(0), so a voltage shock with unchanged resting gates is not representable through Config alone.',
     'evidence':'V0=-50mV,I=0: automatic gates produce zero spikes; same V0 with gates from-65mV produces one spike, peak40.4148mV by independent DOP853.',
     'recommendation':'Expose optional m0/h0/n0 for voltage-shock/clamp protocols, while keeping current equilibrium initialization as default.'},
    {'id':'NEURON-N3','priority':'scope note',
     'location':'systems/neuro_astro/model.cuh:93-105; systems/neuro_astro/parameters.inc:3-9',
     'claim':'HH gating rates are the classical6.3C rates; no temperature/Q10 parameter is implemented.',
     'evidence':'Hodgkin-Huxley1952p519 specifies these rates at6.3C and Q10=3 for other temperatures.',
     'recommendation':'State6.3C explicitly when describing the HH model; do not interpret it as calibrated cortical dynamics at37C.'}
]
data['review']={'equation_or_unit_error_found':False,'total_new_successful_CLI_runs':data['new_cli_runs']+boundary['successful_native_runs'],
                'invalid_initial_configs_rejected':boundary['rejected_native_configs'],
                'all_numerical_count_checks_pass':True,'all_sampled_HH_gates_physical':True,
                'issues':issues,'boundary_checks_file':'boundary_findings.json'}
write_json(HERE/'findings.json',data)
paths=[p for p in HERE.iterdir() if p.is_file() and p.name!='manifest.json']
paths+=list((HERE/'runs').glob('*'))
write_json(HERE/'manifest.json',{'cli_sha256':sha(CLI),'files':[
    {'path':str(p.relative_to(HERE)),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(paths)]})
print(json.dumps(data['review'],indent=2))
