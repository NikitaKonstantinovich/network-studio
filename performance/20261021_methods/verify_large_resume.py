"""Compare a new-process continuation (8 -> 6 workers) with measured full runs."""
import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--cli', type=Path, required=True)
p.add_argument('--measurements', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
started = time.perf_counter()
commands = []
pairs = 0
for method in ('adaptive-rk4', 'rk4', 'euler'):
    config = a.measurements / (method + '_500.json')
    full = a.measurements / ('short_' + method + '_500_1')
    pause, resumed = a.output / (method + '_pause.nscp'), a.output / (method + '_resumed.nscp')
    for name, options in (
        (method + '_pause', ['--config', config, '--threads', 8, '--stop-at', '.05', '--checkpoint', pause]),
        (method + '_resumed', ['--resume', pause, '--threads', 6, '--stop-at', '.1', '--checkpoint', resumed]),
    ):
        command = [str(a.cli.resolve()), *map(str, options), '--output', str(a.output / name)]
        r = subprocess.run(command, capture_output=True, encoding='utf-8')
        commands.append(command)
        (a.output / (name + '.log')).write_text(r.stdout + r.stderr, encoding='utf-8')
        assert r.returncode == 0, r.stdout + r.stderr
    assert resumed.read_bytes() == (full / 'final.nscp').read_bytes()
    for csv in full.glob('result_*.csv'):
        assert csv.read_bytes() == (a.output / csv.name.replace('result_', method + '_resumed_')).read_bytes()
        pairs += 1
(a.output / 'report.json').write_text(json.dumps(dict(commands=commands, methods=3, exact_csv_pairs=pairs,
    elapsed_s=time.perf_counter()-started, cli_sha256=hashlib.sha256(a.cli.read_bytes()).hexdigest()), indent=2), encoding='utf-8')
print(f'PASS all 3 methods: new process, 500 neurons, 15041 TM, threads 8 -> 6, {pairs} exact CSV pairs')
