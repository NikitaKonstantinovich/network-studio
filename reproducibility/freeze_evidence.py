"""Keep compact audit evidence and runnable audit sources beside the report.

Large CSVs remain in verification/repro_labs_20261012. The copied audit
sources have the same depth below network_studio as the originals, so their
relative imports still resolve. Re-running them writes to audit_sources.
"""
import hashlib
import json
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
AUDIT = HERE.parent / 'verification/repro_labs_20261012'


def main():
    network = json.loads((AUDIT / 'lab3/network_findings.json').read_text(encoding='utf-8'))
    assert len(network['runs']) == 3, 'Finish all three network experiments before freezing evidence.'
    ftz_path = AUDIT / 'lab3/ftz_findings.json'
    if ftz_path.is_file():
        ftz = json.loads(ftz_path.read_text(encoding='utf-8'))
        assert len(ftz['runs']) == 4, 'Finish the diagnostic controls before freezing their evidence.'
        assert all(all(r['comparison'][kind]['available'] for kind in ('events', 'trace', 'weights'))
                   for r in ftz['runs']), 'Refresh comparisons against all completed production results.'
    for lab in ('lab1', 'lab2', 'lab3'):
        assert (AUDIT / lab / 'findings.json').is_file()
    entries = []

    def copy(source, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        entries.append({'source': str(source), 'copy': destination.relative_to(HERE).as_posix(),
                        'bytes': source.stat().st_size,
                        'sha256': hashlib.sha256(source.read_bytes()).hexdigest()})

    for filename in ('provenance.json', 'ctest.log'):
        copy(AUDIT / filename, HERE / 'evidence' / filename)
    for lab in ('lab1', 'lab2', 'lab3'):
        for source in sorted((AUDIT / lab).glob('*')):
            if source.is_file() and source.suffix == '.json':
                if 'progress' not in source.name:
                    copy(source, HERE / 'evidence' / lab / source.name)
            elif source.is_file() and source.suffix == '.py':
                copy(source, HERE / 'audit_sources' / lab / source.name)
    for filename in ('launcher.cpp', 'CMakeLists.txt', 'README.md'):
        source = AUDIT / 'lab3/ftz' / filename
        if source.is_file():
            copy(source, HERE / 'audit_sources/lab3/ftz' / filename)
    manifest = {'audit_directory': str(AUDIT), 'entries': entries,
                'note': 'CSV data remain in the audit directory; hashes and findings are preserved here.'}
    (HERE / 'evidence/manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(f'Preserved {len(entries)} files; {sum(e["bytes"] for e in entries):,} bytes.')


if __name__ == '__main__':
    main()
