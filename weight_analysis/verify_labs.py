"""Read-only weight reanalysis; no model integration and no checkpoint bypass."""
import argparse
import bisect
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--labs', type=Path, default=ROOT.parent / 'labs')
p.add_argument('--output', type=Path)
args = p.parse_args()
out = (args.output or ROOT / 'verification/weight_snapshots_20261020' / ('labs_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))).resolve()
assert out != args.labs.resolve() and args.labs.resolve() not in out.parents
out.mkdir(parents=True, exist_ok=False)
cli = ROOT / 'build/Release/network_studio_cli.exe'
paths = [args.labs / f'lab6_{hz}Hz{suffix}' for hz in (1, 10) for suffix in ('_experiment.json', '_summary.json')]
paths.append(args.labs / 'lab6_comparison/weights_statistics.json')
def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()
def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))
before = {str(path): sha(path) for path in paths}
start = time.perf_counter()
durations = {}
def run(name, *arguments):
    begin = time.perf_counter()
    result = subprocess.run([str(cli), *map(str, arguments)], capture_output=True)
    durations[name] = time.perf_counter() - begin
    (out / f'{name}.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name}: exit {result.returncode}; see log')
configs = [read(paths[0]), read(paths[2])]
summaries = [read(paths[1]), read(paths[3])]
assert configs[0]['synapses'] == configs[1]['synapses'], 'Branches use different initial edges'
snapshots = [out/'initial.nsweights', out/'1Hz.nsweights', out/'10Hz.nsweights']
run('initial', '--weights-initial', paths[0], '--output', snapshots[0])
for hz, source, dest in zip((1, 10), (paths[1], paths[3]), snapshots[1:]):
    run(f'import_{hz}', '--weights-from-summary', source, '--output', dest)
edges = configs[0]['synapses']
values = [[e['weight'] for e in edges], *[s['final_synapse_weights'] for s in summaries]]
def inhibitory(e):
    return e['reversal'] <= -40 if e['model'] == 'conductance_stp' else (not e['stdp'] and e['weight'] < 0)
def close(a, b):
    assert math.isclose(a, b, rel_tol=3e-13, abs_tol=2e-12), (a, b)
def check_delta(name, report, target, chosen, mode):
    transform = abs if mode == 'absolute' else lambda x: x
    expected = {i: transform(values[target][i])-transform(values[0][i]) for i in chosen}
    rows = list(csv.DictReader((out/f'{name}_changes.csv').open(encoding='utf-8')))
    assert len(rows) == len(chosen) and len({r['stable_id'] for r in rows}) == len(rows)
    assert [float(r['rank']) for r in rows] == list(range(1,len(rows)+1))
    magnitudes = []
    for r in rows:
        i = int(r['before_index'])
        assert int(r['after_index']) == i and i in expected
        assert float(r['raw_delta']) == values[target][i]-values[0][i]
        assert float(r['display_delta']) == expected[i]
        magnitudes.append(abs(float(r['display_delta'])))
    assert magnitudes == sorted(magnitudes,reverse=True)
    nums = list(expected.values())
    actual = report['difference']['statistics']
    assert actual['count'] == len(nums)
    for field, value in [('sum',math.fsum(nums)),('mean',statistics.fmean(nums)),('median',statistics.median(nums)),('stddev_population',statistics.pstdev(nums)),('min',min(nums)),('max',max(nums))]:
        close(actual[field], value)
    histogram = list(csv.DictReader((out/f'{name}_delta_histogram.csv').open(encoding='utf-8')))
    boundaries = [float(r['left']) for r in histogram] + [float(histogram[-1]['right'])]
    counts = [0]*len(histogram)
    for v in nums:
        counts[min(len(counts)-1,max(0,bisect.bisect_right(boundaries,v)-1))] += 1
    assert [int(r['count']) for r in histogram] == counts
results = {}
for mode in ('absolute', 'signed'):
    for polarity in ('all', 'E', 'I'):
        name = f'{mode}_{polarity}'
        arguments = ['--weights-analyze']
        for snapshot in snapshots:
            arguments += ['--snapshot', snapshot]
        run(name, *arguments, '--representation', mode, '--polarity', polarity, '--bins', 40, '--tolerance', 1e-6, '--reference', 0, '--target', 1, '--output', out/name)
        report = read(out / f'{name}_statistics.json')
        chosen = [i for i, e in enumerate(edges) if polarity == 'all' or (polarity == 'I') == inhibitory(e)]
        if name == 'absolute_all':
            raw_rows = list(csv.DictReader((out/f'{name}_weights.csv').open(encoding='utf-8')))
            assert len(raw_rows) == len(edges)*3
            for row in raw_rows:
                k, i = int(row['snapshot']), int(row['row_0based'])
                assert float(row['raw_weight']) == values[k][i]
                assert float(row['display_value']) == abs(values[k][i])
                assert row['selected'] == '1' and row['stable_id']
        for index, raw in enumerate(values):
            nums = [abs(raw[i]) if mode == 'absolute' else raw[i] for i in chosen]
            actual = report['snapshots'][index]['statistics']
            assert actual['count'] == len(nums)
            for field, expected in [('sum', math.fsum(nums)), ('mean', statistics.fmean(nums)), ('median', statistics.median(nums)), ('stddev_population', statistics.pstdev(nums)), ('min', min(nums)), ('max', max(nums))]:
                close(actual[field], expected)
            learning = [i for i in chosen if edges[i]['stdp']]
            assert actual['learning_count'] == len(learning)
            assert actual['at_lower'] == sum(abs(raw[i]-edges[i]['weight_min']) <= 1e-6 for i in learning)
            assert actual['at_upper'] == sum(abs(raw[i]-edges[i]['weight_max']) <= 1e-6 for i in learning)
        hist = list(csv.DictReader((out/f'{name}_histograms.csv').open(encoding='utf-8')))
        boundaries = [float(r['left']) for r in hist[:40]] + [float(hist[39]['right'])]
        for k, raw in enumerate(values):
            expected = [0]*40
            for i in chosen:
                v = abs(raw[i]) if mode == 'absolute' else raw[i]
                expected[min(39, max(0, bisect.bisect_right(boundaries, v)-1))] += 1
            group = hist[k*40:(k+1)*40]
            assert [float(r['left']) for r in group] == boundaries[:-1]
            assert [int(r['count']) for r in group] == expected
        if polarity == 'I':
            assert report['difference']['exactly_unchanged_raw'] == len(chosen)
            assert all(values[0][i] == values[k][i] for k in (1, 2) for i in chosen), 'Inhibitory weights changed'
        check_delta(name, report, 1, chosen, mode)
        results[name] = report
# Both branches checked by the program's identity-based delta, not only Python.
run('I_10Hz', '--weights-analyze', '--snapshot', snapshots[0], '--snapshot', snapshots[2], '--polarity', 'I', '--output', out/'I_10Hz')
assert read(out/'I_10Hz_statistics.json')['difference']['exactly_unchanged_raw'] == 3032
assert all(float(row['raw_delta']) == 0 for row in csv.DictReader((out/'I_10Hz_changes.csv').open(encoding='utf-8')))
run('all_10Hz', '--weights-analyze', '--snapshot', snapshots[0], '--snapshot', snapshots[2], '--output', out/'all_10Hz')
check_delta('all_10Hz',read(out/'all_10Hz_statistics.json'),2,list(range(len(edges))),'signed')
previous = list(read(paths[-1]).values())
for i, previous_snapshot in enumerate(previous):
    for polarity in ('all', 'E', 'I'):
        actual = results[f'absolute_{polarity}']['snapshots'][i]['statistics']
        for field in ('mean', 'median', 'min', 'max'):
            close(actual[field], previous_snapshot[polarity][field])
        close(actual['sum'], previous_snapshot[polarity]['sum_abs'])
assert before == {str(path): sha(path) for path in paths}, 'Lab data changed'
manifest = {'description': __doc__, 'inputs_unchanged': True, 'input_sha256': before, 'cli_sha256': sha(cli), 'simulation_steps_executed': 0, 'total_wall_seconds': time.perf_counter()-start, 'command_seconds': durations, 'synapses': len(edges), 'inhibitory_exactly_unchanged_each_branch': 3032, 'results': results}
(out/'verification.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(out)
print(json.dumps({'seconds':manifest['total_wall_seconds'],'links':len(edges),'unchanged_I':3032}))
