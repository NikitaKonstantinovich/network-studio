"""Record the exact local inputs of the laboratory reproducibility audit."""
import datetime
import hashlib
import importlib.metadata
import json
import platform
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parent
REPO = STUDIO.parent / 'cudaynamics'
WORKSPACE = REPO.parent
AUDIT = STUDIO / 'verification/repro_labs_20261012'
PDF = Path('C:/Users/N1kit/Downloads/UMP_Lobov_i_dr_Neyrokiberneticheskie_printsipy_fin.pdf')


def entry(path):
    path = Path(path)
    return {'path': str(path.resolve()), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def git(*args):
    return subprocess.check_output(['git', '-c', f'safe.directory={REPO.as_posix()}',
                                    '-C', str(REPO), *args], text=True).strip()


def main():
    AUDIT.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(WORKSPACE / 'tmp/python_deps'))
    names = ['network.h', 'network.cpp', 'io.h', 'io.cpp', 'cli.cpp', 'gui.cpp',
             'generator.h', 'generator.cpp', 'lab2/population.h', 'lab2/population.cpp',
             'build/Release/network_studio_cli.exe', 'build/Release/network_studio.exe',
             'build/Release/network_studio_population.exe',
             'vendor/systems/neuro_astro/model.cuh', 'CMakeLists.txt',
             'lab1/config_compat.py', 'lab1/reproduce.py', 'lab1/neuronet_probe.py',
             'lab2/rings.py', 'lab2/populations.py', 'lab2/check_metrics.py',
             'lab3/reference_tm.py', 'lab3/extract_neuronet_reference.py']
    reports = [WORKSPACE / f'labs/lab{i}_report/Отчёт_ЛР{i}.md' for i in (1, 2, 3)]
    manifest = {
        'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'scope': 'Laboratory works 1 through 3, current native configuration engine',
        'git_commit': git('rev-parse', 'HEAD'), 'git_branch': git('branch', '--show-current'),
        'working_tree_is_dirty': bool(git('status', '--porcelain')),
        'platform': platform.platform(), 'python': sys.version,
        'python_packages': {name: importlib.metadata.version(name) for name in
                            ('numpy', 'scipy', 'matplotlib', 'pillow', 'pymupdf', 'reportlab')},
        'manual': entry(PDF), 'user_reports': [entry(p) for p in reports],
        'implementation': [entry(STUDIO / p) for p in names],
        'ctest_log': entry(AUDIT / 'ctest.log'),
        'notes': ['Runs use the current working-tree build; commit alone does not identify it.',
                  'Existing user data and historical lab results are not overwritten.',
                  'A matching CSV hash establishes replay agreement, not independent numerical or biological validity.']
    }
    (AUDIT / 'provenance.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'branch': manifest['git_branch'], 'files': len(manifest['implementation']),
                      'cli_sha256': entry(STUDIO / 'build/Release/network_studio_cli.exe')['sha256']}, ensure_ascii=False))
    import pymupdf
    render_dir = AUDIT / 'manual_pages'
    render_dir.mkdir(exist_ok=True)
    with pymupdf.open(PDF) as document:
        for number in (19, 20, 22, 23, 24):
            document[number-1].get_pixmap(matrix=pymupdf.Matrix(1.4, 1.4)).save(render_dir / f'page_{number}.png')
    print('Provenance recorded and relevant manual pages rendered.')


if __name__ == '__main__':
    main()
