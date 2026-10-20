"""Sequential control runs; bounded RAM in both modes, same model and sampling.
These are small-network controls, never a 150/400-second laboratory validation.
"""
import datetime
import json
import pathlib
import statistics
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'verification' / 'disk_history_20261020' / ('measure_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
OUT.mkdir(parents=True)
rows = []
start = time.perf_counter()
for duration in (0.5, 10.0):
    for repeat in range(3):
        for mode in ('ram', 'disk'):
            target = OUT / f'{duration}_{repeat}_{mode}'
            result = subprocess.run([str(ROOT / 'build/Release/network_studio_archive_tests.exe'), '--bench', str(target), str(duration), mode], text=True, capture_output=True, check=True)
            row = json.loads(result.stdout)
            row['repeat'] = repeat
            rows.append(row)
            (OUT / 'measurements.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
            print(row, flush=True)
summary = []
for duration in (0.5, 10.0):
    for mode in ('ram', 'disk'):
        values = [r for r in rows if r['model_seconds'] == duration and r['mode'] == mode]
        times = [r['wall_seconds'] for r in values]
        summary.append(dict(model_seconds=duration, mode=mode, median_seconds=statistics.median(times), min_seconds=min(times), max_seconds=max(times), peak_working_set_bytes=max(r['peak_working_set_bytes'] for r in values), archive_bytes=max(r['archive_bytes'] for r in values)))
(OUT / 'summary.json').write_text(json.dumps(dict(controls=summary, total_wall_seconds=time.perf_counter()-start), indent=2), encoding='utf-8')
print(OUT)
