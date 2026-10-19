"""Sequential GUI/runner/CLI verification on one final build; no execution without --execute."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from measure import HERE, ROOT, compare, run_process, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--new-dir', type=Path, default=ROOT / 'build/parallel/Release')
    p.add_argument('--threads', type=int, choices=(0, 2, 4, 8), default=0)
    p.add_argument('--execute', action='store_true')
    a = p.parse_args()
    out, binaries = a.out.resolve(), a.new_dir.resolve()
    fixture = HERE / 'fixtures/burst_50ms.json'
    selected = 'auto' if a.threads == 0 else str(a.threads)
    plan = [('direct_1', [str(binaries / 'network_studio_benchmark.exe'), str(fixture), str(out / 'direct_1'), '1'])]
    for count in ('1', selected):
        name = 'gui_' + count
        plan.append((name, [str(binaries / 'network_studio.exe'), '--config', str(fixture), '--benchmark', str(out / name), '--display-hz', '10', '--compute-threads', count]))
    for hz in (10, 1, 60):
        name = f'runner_hz{hz}'
        plan.append((name, [str(binaries / 'network_studio_runner_benchmark.exe'), str(fixture), str(out / name), str(hz), '1', str(a.threads)]))
    cli = str(binaries / 'network_studio_cli.exe')
    plan.append(('cli_prefix_2', [cli, '--config', str(fixture), '--threads', '2', '--stop-at', '.025', '--checkpoint', str(out / 'cli_prefix_2/final.nscp'), '--output', str(out / 'cli_prefix_2/result')]))
    plan.append(('cli_resume_8', [cli, '--resume', str(out / 'cli_prefix_2/final.nscp'), '--threads', '8', '--checkpoint', str(out / 'cli_resume_8/final.nscp'), '--output', str(out / 'cli_resume_8/result')]))
    if not a.execute:
        print(json.dumps(plan, indent=2)); return
    lock = HERE.parent / '.benchmark.lock'
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        with os.fdopen(fd, 'w') as f: f.write(str(os.getpid()))
        out.mkdir(parents=True, exist_ok=False)
        result = {'started_utc': datetime.now(timezone.utc).isoformat(), 'fixture': str(fixture), 'fixture_sha256': sha(fixture),
                  'model_duration': .05, 'runs': [], 'note': 'Sequential same-build comparisons. GUI uses hidden native HWND/Present(1), not a manual interaction session. Memory includes setup and export.'}
        for name, cmd in plan:
            target = out / name; target.mkdir()
            print('Starting ' + name, flush=True)
            wall, memory = run_process(cmd, target, .1)
            timing_path = target / ('timing.json' if name == 'direct_1' else 'benchmark.json')
            timing = json.loads(timing_path.read_text()) if timing_path.exists() else None
            row = {'name': name, 'command': cmd, 'exe_sha256': sha(Path(cmd[0])), 'process_seconds': wall,
                   'integrated_seconds': .025 if name.startswith('cli_') else .05,
                   'start_model_seconds': .025 if name == 'cli_resume_8' else 0,
                   'timing': timing, 'memory': {k: v for k, v in memory.items() if k != 'timeline'}}
            if name not in ('direct_1', 'cli_prefix_2'):
                row['comparison_equal'] = compare(out / 'direct_1', target, 'exact')['equal']
            result['runs'].append(row)
            (out / 'integration.json').write_text(json.dumps(result, indent=2) + '\n')
            print(f'Finished {name}: {wall:.3f}s whole process', flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
