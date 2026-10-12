"""Reproduce the historical counterexamples without modifying old audit evidence."""
import contextlib
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'verification/numerics_20261012/baseline'
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT.parent / 'tmp/python_deps'))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    cli = OUT / 'Release/network_studio_cli.exe'
    assert cli.is_file(), cli
    evidence = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (ROOT / 'model_audit').rglob('*') if p.is_file()}
    (OUT / 'old_audit_sha256.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    integration = module('original_integration_audit', ROOT / 'model_audit/integration/audit.py')
    integration.HERE = OUT / 'integration'
    integration.HERE.mkdir(exist_ok=True)
    integration.CLI = cli
    with (OUT / 'integration.log').open('w', encoding='utf-8') as log, contextlib.redirect_stdout(log):
        integration.main()
    neurons = module('original_neuron_audit', ROOT / 'model_audit/neurons/audit.py')
    neurons.HERE = OUT
    neurons.CLI = cli
    base = json.loads((ROOT / 'model_audit/neurons/base.json').read_text(encoding='utf-8'))
    base.update(duration=.2, dt=25e-6, sample=50e-6, noise_sigma=0)
    base['neurons'][0].update(model='izhikevich', voltage=-65, current=10, amplitude=0,
                              izh_a=.1, izh_b=.2, izh_c=-65, izh_d=2)
    ref = neurons.reference(base)
    times, values, events, summary = neurons.native(base, 'izh_FS_dt25us')
    result = dict(cli_sha256=hashlib.sha256(cli.read_bytes()).hexdigest(),
                  native_spikes=len(events), reference_spikes=len(ref['event_times_ms']),
                  maximum_event_error_ms=max(abs(a-b) for a,b in zip(events, ref['event_times_ms'])),
                  native_events_ms=events, reference_events_ms=ref['event_times_ms'],
                  final_native_state=values[:, -1].tolist(), final_reference_state=ref['final_state'])
    (OUT / 'fs_findings.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if not isinstance(v, list)}, indent=2))
    stimulus = json.loads((OUT / 'integration/findings.json').read_text())
    print('25 us charge ratios:', [r['delivered_charge_ratio'] for r in stimulus['rows'] if r['dt_us']==25])
    print('25 us decay:', stimulus['decay_rows'][0])


if __name__ == '__main__':
    main()
