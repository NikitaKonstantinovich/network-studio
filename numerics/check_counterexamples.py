"""Repeat the archived analytic counterexamples against the corrected CLI."""
import contextlib
import csv
import importlib.util
import json
import math
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'verification/numerics_20261012/current/integration'
OUT.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('old_integration',ROOT/'model_audit/integration/audit.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
audit.HERE=OUT;audit.CLI=ROOT/'build/Release/network_studio_cli.exe'
with (OUT/'run.log').open('w',encoding='utf-8') as log,contextlib.redirect_stdout(log):audit.main()
data=json.loads((OUT/'findings.json').read_text())
for row in data['rows']:
    assert abs(row['delivered_charge_ratio']-1)<1e-9,row
for row in data['decay_rows']:
    x,expected=row['final_Y_uM'],row['exact_final_Y_uM']
    assert x>0 and expected>0 and abs(math.log(x)-math.log(expected))<1e-9,row
    assert not row['grew_after_release'],row
    row['log_error']=abs(math.log(x)-math.log(expected))
    label=f"decay_dt{row['dt_us']:g}".replace('.','p')
    with (OUT/'raw'/f'{label}_events.csv').open(encoding='utf-8',newline='') as stream:
        event=next(csv.DictReader(stream))
    row['first_sample_after_release_s']=row.pop('release_time_s')
    row['actual_release_time_s']=float(event['time_s'])
    row['exact_from_event_uM']=500*math.exp(-200000*(.001-row['actual_release_time_s']))
    row['log_error_from_event']=abs(math.log(x)-math.log(row['exact_from_event_uM']))
    assert row['log_error_from_event']<1e-8
data.update(finding='All eight capacitor controls deliver the exact charge.',
            decay_finding='All four fast-decay controls remain positive and agree in logarithmic error below 1e-9.',
            code=['network.cpp:next_input_boundary','integrator.cpp:exact_flow','numerics/integrator.h:Flow'])
(OUT/'findings.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
print('PASS: 8 capacitor controls and 4 tiny-positive logarithmic decay controls.')
print(json.dumps(data['decay_rows'][0],indent=2))
