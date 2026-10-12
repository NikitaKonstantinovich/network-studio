"""Collect exact restart evidence and optionally compare the pre-checkpoint CLI.

Run AFTER build.ps1 -Test -Smoke. Python stdlib only; no numerical tolerance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--previous-release', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = {'restart_cases': [], 'baseline_cases': [], 'binaries': {}}
    source = root / 'build/native/checkpoint-process'
    evidence = out / 'processes'
    shutil.copytree(source, evidence, dirs_exist_ok=True)
    suffixes = ['.nscp', '_trace.csv', '_weights.csv', '_events.csv', '_summary.json', '_experiment.json']
    for i in range(5):
        base = evidence / f'case{i}'
        equality = {}
        for suffix in suffixes:
            left, right = Path(str(base) + '-full' + suffix), Path(str(base) + '-resumed' + suffix)
            assert left.read_bytes() == right.read_bytes(), (i, suffix)
            equality[suffix] = sha(left)
        summary = json.loads(Path(str(base) + '-resumed_summary.json').read_text(encoding='utf-8'))
        partial = json.loads(Path(str(base) + '-part_summary.json').read_text(encoding='utf-8'))
        partial2 = json.loads(Path(str(base) + '-part2_summary.json').read_text(encoding='utf-8'))
        results['restart_cases'].append({'case': i, 'stop_times': [partial['time_s'], partial2['time_s']],
                                         'final': summary, 'equal_sha256': equality})
    if args.previous_release:
        previous = out / 'previous_cli.exe'
        with zipfile.ZipFile(args.previous_release) as archive:
            name = next(n for n in archive.namelist() if n.endswith('/network_studio_cli.exe'))
            previous.write_bytes(archive.read(name))
        old_manifest = json.loads((root / 'numerics/build_manifest.json').read_text(encoding='utf-8'))
        expected = next(v for k, v in old_manifest['binaries_sha256'].items() if k.endswith('network_studio_cli.exe'))
        assert sha(previous) == expected
        for i in range(5):
            prefix = out / f'previous_case{i}'
            p = subprocess.run([str(previous), '--config', str(evidence / f'case{i}.json'), '--output', str(prefix)],
                               capture_output=True, text=True)
            (out / f'previous_case{i}.log').write_text(p.stdout + p.stderr, encoding='utf-8')
            assert p.returncode == 0, p.stderr
            for suffix in suffixes[1:]:
                assert Path(str(prefix) + suffix).read_bytes() == (evidence / f'case{i}-full{suffix}').read_bytes(), (i, suffix)
            results['baseline_cases'].append({'case': i, 'exports_byte_identical': True})
        results['previous_cli_sha256'] = sha(previous)
    for name in ['network_studio.exe', 'network_studio_cli.exe']:
        path = root / 'build/Release' / name
        results['binaries'][str(path)] = sha(path)
    results['solver_build_header'] = (root / 'build/native/checkpoint_build.h').read_text().strip()
    historical = out / 'historical_files_before.json'
    if historical.exists():
        items = json.loads(historical.read_text(encoding='utf-8-sig'))
        for item in items:
            assert sha(Path(item['Path'])) == item['Hash'].lower(), item['Path']
        results['historical_files_unchanged'] = len(items)
    (out / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f"PASS: {len(results['restart_cases'])} exact process restart cases; "
          f"{len(results['baseline_cases'])} baseline comparisons. {out / 'results.json'}")


if __name__ == '__main__':
    main()
