"""Sequential measurements, no alteration of scientific inputs except method/dt.

short creates a new output tree and runs 3 repeats of 0..0.1 s; long reuses
its hashed copies for 0..10 s. Loading/export/checkpoint are timed separately.
"""
import argparse
import bisect
import csv
import hashlib
import json
import platform
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
METHODS = ('adaptive-rk4', 'rk4', 'euler')


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def read(p):
    return json.loads(p.read_text(encoding='utf-8'))


def rows(p):
    with p.open(encoding='utf-8') as f:
        return list(csv.DictReader(f))


def differences(reference, current):
    # Full final-state/learning arrays, not only selected recording channels.
    result = {}
    for file, keys in [('state.csv', ('value',)), ('plasticity.csv', ('weight', 'pre', 'post', 'slow'))]:
        left, right = rows(reference / file), rows(current / file)
        assert [x['index'] for x in left] == [x['index'] for x in right]
        for key in keys:
            result['max_' + key] = max((abs(float(a[key])-float(b[key])) for a, b in zip(left, right)), default=0)
    def events(path):
        data = {}
        for e in rows(path / 'result_events.csv'):
            if e['type'] == 'spike':
                data.setdefault(e['node_index_1based'], []).append(float(e['time_s']))
        return data
    left, right = events(reference), events(current)
    result['same_spike_counts'] = {n: len(v) for n, v in left.items()} == {n: len(v) for n, v in right.items()}
    result['max_paired_spike_s'] = max((abs(a-b) for n in left for a, b in zip(left[n], right.get(n, []))), default=0)
    result['paired_spikes_note'] = 'Per-neuron ordinal pairs; count mismatches reported separately.'
    trace_a, trace_b = rows(reference / 'result_trace.csv'), rows(current / 'result_trace.csv')
    assert [r['time_s'] for r in trace_a] == [r['time_s'] for r in trace_b]
    def far(times, t):
        index = bisect.bisect_left(times, t)
        return all(abs(v-t) >= .002 for v in times[max(0,index-1):index+1])
    result['voltage_exclusion_s'] = .002
    result['max_recorded_voltage_away_resets_mV'] = 0
    result['compared_voltage_samples'] = 0
    for column in trace_a[0]:
        if not column.startswith('N') or not column.endswith('_V_mV'):
            continue
        neuron = column[1:column.index('_')]
        for a, b in zip(trace_a, trace_b):
            t = float(a['time_s'])
            if far(left.get(neuron, []), t) and far(right.get(neuron, []), t):
                result['compared_voltage_samples'] += 1
                result['max_recorded_voltage_away_resets_mV'] = max(result['max_recorded_voltage_away_resets_mV'], abs(float(a[column])-float(b[column])))
    # A rolling delivery journal cannot be treated as a complete 10 s history.
    # Short windows retain all deliveries, so ordinal comparison is meaningful.
    if float(trace_a[-1]['time_s']) <= .1:
        def arrivals(path):
            data = {}
            for e in rows(path / 'result_deliveries.csv'):
                data.setdefault(e['synapse_index_1based'], []).append(float(e['t_arrival_s']))
            return data
        dl, dr = arrivals(reference), arrivals(current)
        result['same_delivery_counts'] = {k: len(v) for k, v in dl.items()} == {k: len(v) for k, v in dr.items()}
        result['max_paired_arrival_s'] = max((abs(a-b) for k in dl for a, b in zip(dl[k], dr.get(k, []))), default=0)
    else:
        result['delivery_comparison'] = 'Full cumulative counts in timing; bounded delivery journals are not paired as complete histories.'
    # Actual stimulus clocks must be identical, even for different methods.
    result['stimulus_csv_exact'] = all((reference / ('result_' + suffix + '.csv')).read_bytes() ==
                                     (current / ('result_' + suffix + '.csv')).read_bytes()
                                     for suffix in ('stimuli', 'shared_stimuli'))
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['short', 'long', 'report'])
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--config', type=Path, default=Path('E:/asp/26_27/labs/lab6_config_initial.json'))
    p.add_argument('--exe', type=Path, default=ROOT / 'build/Release/network_studio_method_benchmark.exe')
    p.add_argument('--threads', type=int, default=8)
    a = p.parse_args()
    if a.mode == 'short':
        a.output.mkdir(parents=True, exist_ok=False)
        c = read(a.config)
        assert len(c['neurons']) == 500 and len(c['synapses']) == 15041
        assert not c['astrocytes'] and c['noise_sigma'] == 3
        meta = dict(source=str(a.config.resolve()), source_sha256=sha(a.config), executable=str(a.exe.resolve()),
                    executable_sha256=sha(a.exe), threads=a.threads, hardware=platform.processor(),
                    preserved_animat=c.get('animat', 'absent in source: legacy disabled default'),
                    model_seconds_short=.1, model_seconds_long=10, copies={}, commands=[])
        for method in METHODS:
            for dt in (25, 50, 100, 250, 500):
                if dt == 25 and method != 'adaptive-rk4':
                    continue
                copied = dict(c, dt=dt / 1e6, integration_method=method)
                assert {k: v for k, v in copied.items() if k not in ('dt', 'integration_method')} == {
                    k: v for k, v in c.items() if k not in ('dt', 'integration_method')}
                name = f'{method}_{dt}'
                path = a.output / (name + '.json')
                path.write_text(json.dumps(copied, ensure_ascii=False, indent=2), encoding='utf-8')
                meta['copies'][name] = sha(path)
        (a.output / 'manifest.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
    meta = read(a.output / 'manifest.json')
    if a.mode != 'report':
        assert sha(a.exe) == meta['executable_sha256'], 'Benchmark binary changed'
        names = list(meta['copies']) if a.mode == 'short' else [f'{m}_500' for m in METHODS]
        repeats = 3 if a.mode == 'short' else 1
        stop = .1 if a.mode == 'short' else 10
        started = time.perf_counter()
        for name in names:
            config = a.output / (name + '.json')
            assert sha(config) == meta['copies'][name]
            for repeat in range(1, repeats + 1):
                out = a.output / f'{a.mode}_{name}_{repeat}'
                command = [str(a.exe.resolve()), str(config.resolve()), str(out.resolve()), str(meta['threads']), str(stop)]
                meta['commands'].append(command)
                (a.output / 'manifest.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
                with (a.output / (out.name + '.log')).open('w', encoding='utf-8') as log:
                    r = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
                if r.returncode:
                    raise RuntimeError(f'{name} failed; see {out.name}.log')
                print(out.name, read(out / 'timing.json')['solve_s'], flush=True)
        meta[a.mode + '_wall_s'] = time.perf_counter()-started
        assert sha(Path(meta['source'])) == meta['source_sha256']
        (a.output / 'manifest.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
    report = {'short': [], 'long': []}
    ref = a.output / 'short_adaptive-rk4_25_1'
    for name in meta['copies']:
        paths = sorted(a.output.glob('short_' + name + '_*'))
        timings = [read(p / 'timing.json') for p in paths if p.is_dir() and (p / 'timing.json').exists()]
        if not timings:
            continue
        values = [t['solve_s'] for t in timings]
        for p in paths:
            if p.is_dir():
                assert sha(p / 'final.nscp') == sha(paths[0] / 'final.nscp'), 'Repeated run changed numerical result'
        report['short'].append(dict(name=name, median_s=statistics.median(values), min_s=min(values), max_s=max(values),
                                    measurements=timings, difference_vs_adaptive25=differences(ref, paths[0])))
    for method in METHODS:
        path = a.output / f'long_{method}_500_1'
        if (path / 'timing.json').exists():
            timing = read(path / 'timing.json')
            timing['estimated_1000_s_hours'] = timing['solve_s'] / timing['model_s'] * 1000 / 3600
            fine = a.output / 'long_adaptive-rk4_25_1'
            reference = fine if (fine / 'timing.json').exists() else a.output / 'long_adaptive-rk4_500_1'
            timing['reference'] = reference.name
            timing['difference_vs_reference'] = differences(reference, path)
            report['long'].append(timing)
    fine = a.output / 'long_adaptive-rk4_25_1'
    if (fine / 'timing.json').exists():
        report['long_fine_reference'] = read(fine / 'timing.json')
    (a.output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
