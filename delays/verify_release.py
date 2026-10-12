"""Release evidence: exact old/new zero-delay comparison, independent learning,
and step refinement against an independent adaptive delayed-network solver.

Requires NumPy/SciPy. Run after build.ps1 -Test -Smoke; output in a NEW directory.
"""
import argparse
import copy
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT/'build/Release/network_studio_cli.exe'
spec = importlib.util.spec_from_file_location('delayed_reference', ROOT/'delays/reference.py')
ref = importlib.util.module_from_spec(spec); spec.loader.exec_module(ref)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(cli, *args):
    result = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    return result.stdout


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


def rows(prefix, suffix):
    with Path(str(prefix)+suffix).open(encoding='utf-8') as f:
        return list(csv.DictReader(f))


def payload(blob):
    pos = 26+8
    for _ in range(3):  # algorithm, build fingerprint, configuration JSON
        count, = struct.unpack_from('<Q', blob, pos)
        pos += 8+count
    return blob[pos:-8]  # parameters and all persistent runtime fields


def baseline(out, old):
    results = []
    configs = []
    for i in range(8):
        path = out/f'legacy_preset{i}.json'
        run(old, '--preset', i, '--duration', .3, '--sample', .0001, '--save', path)
        configs.append(path)
    fixture_dir = old.parent
    if not all((fixture_dir/f'case{i}.json').is_file() for i in range(5)):
        fixture_dir = out/'legacy_fixtures'
        run(ROOT/'build/Release/network_studio_checkpoint_tests.exe', '--fixtures', fixture_dir)
        for i in range(5):
            path = fixture_dir/f'case{i}.json'; c = json.loads(path.read_text(encoding='utf-8'))
            c['schema'] = 4
            for edge in c['synapses']:
                edge.pop('delay', None); edge.pop('stdp_timing', None)
            save(path, c)
    for i in range(5): configs.append(fixture_dir/f'case{i}.json')
    for i, config in enumerate(configs):
        a, b = out/f'old{i}', out/f'zero{i}'
        for exe, prefix in ((old, a), (CLI, b)):
            run(exe, '--config', config, '--output', prefix, '--checkpoint', str(prefix)+'.nscp')
        identical = {}
        for suffix in ('_trace.csv', '_weights.csv', '_events.csv'):
            p, q = Path(str(a)+suffix), Path(str(b)+suffix)
            assert p.read_bytes() == q.read_bytes(), (i, suffix)
            identical[suffix] = sha(p)
        prior = payload(Path(str(a)+'.nscp').read_bytes())
        current = payload(Path(str(b)+'.nscp').read_bytes())
        assert current[:len(prior)] == prior, ('full runtime/hidden state/traces differ', i)
        results.append(dict(case=i, runtime_bytes_equal=True, exports=identical))
    return results


def examples(out):
    template = out/'template.json'
    run(CLI, '--preset', 5, '--save', template)
    base = json.loads(template.read_text(encoding='utf-8'))
    transmission = copy.deepcopy(base)
    transmission.update(name='Delayed TM transmission / 5.37 ms', duration=.15, sample=.0001)
    transmission['neurons'][0].update(start=.010, stop=.020)
    transmission['synapses'][0]['delay'] = .00537
    control = copy.deepcopy(base)
    control.update(name='Controlled pair / arrival', duration=.030, sample=.00005)
    control['parameters'].update(g_Na=0, g_K=0, g_L=0, C_m=1, V_spike=0)
    for i, n in enumerate(control['neurons']):
        n.update(model='hodgkin_huxley', voltage=-10-5*i, current=1, amplitude=0,
                 name='Pre capacitor' if i == 0 else 'Post capacitor')
    control['synapses'][0].update(model='exponential_current', stdp=True, gain=0,
        delay=.010, a_plus=.01, a_minus=.012, stdp_timing='arrival')
    emission = copy.deepcopy(control)
    emission['name'] = 'Controlled pair / emission'
    emission['synapses'][0]['stdp_timing'] = 'emission'
    net = copy.deepcopy(base)
    net.update(name='Four neurons / heterogeneous delays / pair arrival', duration=.080, sample=.0001)
    net['neurons'] = [copy.deepcopy(base['neurons'][0]) for _ in range(4)]
    for i, n in enumerate(net['neurons']):
        n.update(name=f'RS {i+1}', px=.2+.6*(i%2), py=.25+.5*(i//2), amplitude=0,
                 current=[10, 7, 5, 3][i], voltage=-65+i)
    net['synapses'] = []
    for source, target, delay in ((0, 1, .00137), (0, 2, .00411), (1, 2, .0000137), (2, 3, .00873), (3, 0, 0)):
        edge = copy.deepcopy(base['synapses'][0])
        edge.update(source=source, target=target, delay=delay, gain=3, stdp=True)
        net['synapses'].append(edge)
    docs = dict(delay_transmission=transmission, stdp_arrival=control, stdp_emission=emission, heterogeneous_delays=net)
    for name, c in docs.items(): save(out/f'{name}.json', c)
    return docs


def audit_learning(config, prefix):
    spikes = [[] for _ in config['neurons']]
    for row in rows(prefix, '_events.csv'):
        if row['type'] == 'spike': spikes[int(row['node_index_1based'])-1].append(float(row['time_s']))
    arrivals = [[] for _ in config['synapses']]
    for row in rows(prefix, '_deliveries.csv'):
        j = int(row['synapse_index_1based'])-1
        assert float(row['t_arrival_s']) == float(row['t_emit_s'])+config['synapses'][j]['delay']
        arrivals[j].append(float(row['t_arrival_s']))
    samples = rows(prefix, '_weights.csv'); worst = 0.
    for j, edge in enumerate(config['synapses']):
        pre = arrivals[j] if edge['stdp_timing'] == 'arrival' else spikes[edge['source']]
        post = spikes[edge['target']]
        timeline = {}
        for t in pre: timeline.setdefault(t, [0, 0])[0] += 1
        for t in post: timeline.setdefault(t, [0, 0])[1] += 1
        events = iter(sorted(timeline.items())); next_event = next(events, None)
        previous_pre, previous_post, weight = [], [], edge['weight']
        for sample in samples:
            time = float(sample['time_s'])
            while next_event is not None and next_event[0] <= time:
                t, (p, q) = next_event
                weight = ref.learning(edge, weight, previous_pre, previous_post, t, p, q)
                previous_pre.extend([t]*p); previous_post.extend([t]*q)
                next_event = next(events, None)
            worst = max(worst, abs(weight-float(sample[f'S{j+1}_weight'])))
    assert worst < 2e-12, (prefix, worst)
    return worst


def convergence(out, docs):
    scenarios = {'transmission': docs['delay_transmission']}
    for rule in ('additive', 'triplet'):
        for mode in ('arrival', 'emission'):
            net = copy.deepcopy(docs['heterogeneous_delays'])
            for edge in net['synapses']: edge.update(stdp_rule=rule, stdp_timing=mode)
            scenarios[f'network_{rule}_{mode}'] = net
    results = []
    for name, c in scenarios.items():
        reference = ref.solve(c)
        # A tighter independent run checks that reference accuracy is sufficient.
        refined = ref.solve(c, rtol=3e-13)
        assert len(reference['events']) == len(refined['events'])
        reference_event_error = max(abs(a[0]-b[0]) for a, b in zip(reference['events'], refined['events']))
        for step in (50e-6, 25e-6, 12.5e-6):
            test = copy.deepcopy(c); test['dt'] = step
            prefix = out/f'{name}_{step*1e6:g}us'; config = Path(str(prefix)+'.json'); save(config, test)
            run(CLI, '--config', config, '--output', prefix)
            samples = rows(prefix, '_trace.csv'); times = np.array([float(r['time_s']) for r in samples])
            actual = np.array([[float(v) for k, v in r.items() if k != 'time_s'] for r in samples])
            expected, expected_weights = reference['sample'](times)
            # All columns are recorded; values have different physical units.
            voltages = list(range(0, 4*len(c['neurons']), 4))
            voltage_error = float(np.max(np.abs(actual[:, voltages]-expected[:, voltages])))
            state_error = float(np.max(np.abs(actual-expected)/(1+np.abs(expected))))
            weight_values = np.array([[float(v) for k, v in r.items() if k != 'time_s'] for r in rows(prefix, '_weights.csv')])
            weight_error = float(np.max(np.abs(weight_values-expected_weights)))
            events = [(float(r['time_s']), int(r['node_index_1based'])-1) for r in rows(prefix, '_events.csv') if r['type'] == 'spike']
            assert [i for t, i in events] == [i for t, i in reference['events']], name
            spike_error = max(abs(a[0]-b[0]) for a, b in zip(events, reference['events']))
            learning_error = audit_learning(test, prefix)
            results.append(dict(scenario=name, dt_us=step*1e6, spikes=len(events), max_spike_error_s=spike_error,
                                max_voltage_error_mV=voltage_error, max_scaled_state_error=state_error,
                                max_weight_error=weight_error, independent_log_recalculation_error=learning_error,
                                reference_refinement_event_error_s=reference_event_error))
            print(name, step, spike_error, voltage_error, weight_error, flush=True)
        coarse, fine = results[-3], results[-1]
        assert fine['max_spike_error_s'] < coarse['max_spike_error_s']
        assert fine['max_voltage_error_mV'] < coarse['max_voltage_error_mV']
        assert fine['max_spike_error_s'] < 2e-7 and fine['max_voltage_error_mV'] < .01
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-cli', type=Path, required=True)
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    results = {'baseline_cli_sha256': sha(args.baseline_cli)}
    results['zero_delay'] = baseline(out, args.baseline_cli.resolve())
    print('PASS: 13 old/new full state and history byte comparisons', flush=True)
    docs = examples(out)
    results['controlled_pairs'] = []
    for name in ('stdp_arrival', 'stdp_emission'):
        prefix = out/name
        run(CLI, '--config', out/f'{name}.json', '--output', prefix)
        results['controlled_pairs'].append(dict(mode=name, log_error=audit_learning(docs[name], prefix),
            final_weight=float(rows(prefix, '_weights.csv')[-1]['S1_weight'])))
    results['convergence'] = convergence(out, docs)
    results['binaries'] = {str(ROOT/'build/Release'/name): sha(ROOT/'build/Release'/name)
                           for name in ('network_studio.exe', 'network_studio_cli.exe')}
    save(out/'results.json', results)
    print('PASS:', out/'results.json')


if __name__ == '__main__': main()
