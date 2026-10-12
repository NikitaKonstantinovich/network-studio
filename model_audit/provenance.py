"""Pin source/binary identity; preserve prior audit evidence without rewriting it."""
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parent
REPO = STUDIO.parent / 'cudaynamics'


def relocated(path):
    """Resolve historical provenance paths without rewriting the evidence."""
    path = Path(path)
    old_studio = REPO / 'standalone/network_studio'
    if path.is_relative_to(old_studio):
        return STUDIO / path.relative_to(old_studio)
    old_model = REPO / 'systems/neuro_astro'
    if path.is_relative_to(old_model):
        return STUDIO / 'vendor/systems/neuro_astro' / path.relative_to(old_model)
    return path


def entry(path):
    return {'path': str(path.resolve()), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    old = json.loads((STUDIO/'reproducibility/evidence/provenance.json').read_text(encoding='utf-8'))
    comparisons = []
    for item in old['implementation']:
        fresh = entry(relocated(item['path']))
        comparisons.append(dict(fresh, matches_lab_audit=fresh['sha256']==item['sha256']))
    extra = [STUDIO/'build/native/network_core.lib', STUDIO/'tests.cpp',
             STUDIO/'vendor/systems/neuro_astro/parameters.inc', STUDIO/'vendor/systems/neuro_astro/variables.inc']
    git = lambda *args: subprocess.check_output(['git', '-c', f'safe.directory={REPO.as_posix()}',
                                                 '-C', str(REPO), *args], text=True).strip()
    result = {'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'branch': git('branch', '--show-current'), 'commit': git('rev-parse', 'HEAD'),
              'working_tree_is_dirty': bool(git('status', '--porcelain')),
              'platform': platform.platform(), 'implementation': comparisons,
              'additional_files': [entry(p) for p in extra],
              'all_previous_implementation_hashes_match': all(x['matches_lab_audit'] for x in comparisons),
              'scope': 'Unmodified current Windows CPU GUI/CLI core; CUDA executable not tested.'}
    (HERE/'provenance.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'matching_previous_files': sum(x['matches_lab_audit'] for x in comparisons),
                      'total_previous_files': len(comparisons)}))


if __name__ == '__main__':
    main()
