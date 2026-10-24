"""Selected methods, strict process resume, legacy JSON and archive metadata."""
import argparse
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--cli', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
out = Path(tempfile.mkdtemp(prefix='methods-', dir=a.work))
commands = []
started = time.perf_counter()


def run(name, options, error=None):
    command = [str(a.cli.resolve()), *map(str, options), '--output', str(out / name)]
    r = subprocess.run(command, capture_output=True, encoding='utf-8')
    (out / (name + '.log')).write_text(r.stdout + r.stderr, encoding='utf-8')
    commands.append(command)
    assert (r.returncode == 0 if error is None else r.returncode != 0 and error in r.stderr), r.stdout + r.stderr


run('base', ['--preset', 5, '--duration', '.035', '--sample', '.001', '--save', out / 'base.json'])
base = json.loads((out / 'base.json').read_text(encoding='utf-8'))
base.update(dt=.0005, noise_sigma=.3)
base['neurons'][0].update(current=10, amplitude=0)
base['synapses'][0].update(delay=.00737, stdp=True)
csv_pairs = 0
for method in ('adaptive-rk4', 'rk4', 'euler'):
    config = dict(base, integration_method=method)
    path = out / f'{method}.json'
    path.write_text(json.dumps(config), encoding='utf-8')
    full, pause, resumed = (out / f'{method}_{suffix}.nscp' for suffix in ('full', 'pause', 'resumed'))
    run(method + '_full', ['--config', path, '--checkpoint', full])
    run(method + '_part', ['--config', path, '--stop-at', '.005', '--checkpoint', pause])
    run(method + '_resumed', ['--resume', pause, '--threads', 12, '--checkpoint', resumed])
    assert full.read_bytes() == resumed.read_bytes()
    for csv in out.glob(method + '_full_*.csv'):
        assert csv.read_bytes() == (out / csv.name.replace('_full_', '_resumed_', 1)).read_bytes()
        csv_pairs += 1
    for flag, value in (('--method', method), ('--dt', .0005)):
        run(method + flag, ['--resume', pause, flag, value], 'configuration overrides')
    summary = json.loads((out / (method + '_full_summary.json')).read_text())
    stats = summary['integration_statistics']
    assert stats['method'] == method
    assert (stats['error_rejections'] is None) == (method != 'adaptive-rk4')
    archive = out / (method + '_archive')
    run(method + '_disk', ['--config', path, '--archive', archive, '--stop-at', '.002'])
    assert json.loads((archive / 'config.json').read_text())['integration_method'] == method
    run(method + '_hh', ['--preset', 0, '--method', method, '--run-for', '.002'], None if method == 'adaptive-rk4' else 'unsupported')
    run(method + '_explicit', ['--preset', 4, '--method', method, '--dt', '.0005', '--sample', '.001', '--run-for', '.002'])
    animat = Path(__file__).resolve().parents[2] / 'neuroanimat/examples/minimal.json'
    run(method + '_animat_full', ['--config', animat, '--method', method, '--dt', '.0005', '--stop-at', '.2', '--checkpoint', out / 'animat_full.nscp'])
    run(method + '_animat_part', ['--config', animat, '--method', method, '--dt', '.0005', '--stop-at', '.0735', '--checkpoint', out / 'animat_pause.nscp'])
    run(method + '_animat_resume', ['--resume', out / 'animat_pause.nscp', '--stop-at', '.2', '--checkpoint', out / 'animat_resumed.nscp'])
    assert (out / 'animat_full.nscp').read_bytes() == (out / 'animat_resumed.nscp').read_bytes()
    for csv in out.glob(method + '_animat_full_*.csv'):
        assert csv.read_bytes() == (out / csv.name.replace('_full_', '_resume_', 1)).read_bytes()
        csv_pairs += 1
legacy = dict(base, schema=9)
legacy.pop('integration_method')
(out / 'legacy.json').write_text(json.dumps(legacy), encoding='utf-8')
run('legacy', ['--config', out / 'legacy.json', '--run-for', '.002'])
assert json.loads((out / 'legacy_summary.json').read_text())['integration_statistics']['method'] == 'adaptive-rk4'
run('unknown', ['--method', 'implicit-euler'], 'Unknown integration method')
report = dict(commands=commands, exact_checkpoint_methods=3, exact_csv_pairs=csv_pairs,
              elapsed_s=time.perf_counter()-started, cli_sha256=hashlib.sha256(a.cli.read_bytes()).hexdigest())
(out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f'PASS methods CLI/process resume/archive: {csv_pairs} exact CSV pairs; {out}')
