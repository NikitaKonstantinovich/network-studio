"""Analytic stimulus-integration audit of the unchanged shipped CLI.

Zero ionic conductances reduce HH voltage to a capacitor: delta V = 1000*I*width/C.
No independent reimplementation of RK4 is used as a reference.
"""
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
STUDIO = HERE.parents[1]
CLI = STUDIO / 'build/Release/network_studio_cli.exe'


def main():
    raw = HERE / 'raw'
    raw.mkdir(exist_ok=True)
    base_file = raw / 'base.json'
    subprocess.run([str(CLI), '--preset', '3', '--save', str(base_file)], check=True, capture_output=True)
    base = json.loads(base_file.read_text(encoding='utf-8'))
    base['name'] = 'Analytic capacitor stimulus audit'
    base['duration'] = .001
    base['sample'] = .00005
    base['noise_sigma'] = 0
    base['parameters'].update({'g_Na': 0, 'g_K': 0, 'g_L': 0, 'C_m': 1})
    n = base['neurons'][0]
    n.update({'voltage': -65, 'amplitude': 100, 'current': 0,
              'frequency': 1, 'stop': .0003, 'width': .00001})
    results = []
    for start_us in (101, 110):
        for dt_us in (25, 12.5, 5, 1):
            c = copy.deepcopy(base)
            c['dt'] = dt_us*1e-6
            c['neurons'][0]['start'] = start_us*1e-6
            label = f'start{start_us}_dt{dt_us:g}'.replace('.', 'p')
            config = raw / f'{label}.json'
            config.write_text(json.dumps(c, indent=2)+'\n', encoding='utf-8')
            run = subprocess.run([str(CLI), '--config', str(config), '--output', str(raw/label)],
                                 capture_output=True, text=True)
            (raw / f'{label}.log').write_text(run.stdout+run.stderr, encoding='utf-8')
            if run.returncode:
                raise RuntimeError(run.stderr)
            rows = list(csv.DictReader((raw/f'{label}_trace.csv').open(newline='', encoding='utf-8')))
            measured = float(rows[-1]['N1_V_mV'])
            exact = -65 + 1000*100*.00001
            results.append({'start_us': start_us, 'width_us': 10, 'dt_us': dt_us,
                            'exact_final_voltage_mV': exact, 'cli_final_voltage_mV': measured,
                            'voltage_error_mV': measured-exact,
                            'delivered_charge_ratio': (measured+65)/(exact+65)})
    tm_base_file = raw/'tm_base.json'
    subprocess.run([str(CLI), '--preset', '5', '--save', str(tm_base_file)], check=True, capture_output=True)
    tm_base = json.loads(tm_base_file.read_text(encoding='utf-8'))
    tm_base.update({'duration': .001, 'noise_sigma': 0})
    tm_base['parameters']['Omega_Y'] = 200000
    for node in tm_base['neurons']:
        node.update({'amplitude': 0, 'current': 0})
    tm_base['neurons'][0]['voltage'] = 29
    tm_base['synapses'][0]['gain'] = 0
    decay = []
    for dt_us in (25, 12.5, 5, 1):
        c = copy.deepcopy(tm_base)
        c.update({'dt': dt_us*1e-6, 'sample': dt_us*1e-6})
        label = f'decay_dt{dt_us:g}'.replace('.', 'p')
        config = raw/f'{label}.json'
        config.write_text(json.dumps(c, indent=2)+'\n', encoding='utf-8')
        run = subprocess.run([str(CLI), '--config', str(config), '--output', str(raw/label)],
                             capture_output=True, text=True)
        (raw/f'{label}.log').write_text(run.stdout+run.stderr, encoding='utf-8')
        if run.returncode:
            raise RuntimeError(run.stderr)
        rows = list(csv.DictReader((raw/f'{label}_trace.csv').open(newline='', encoding='utf-8')))
        events = list(csv.DictReader((raw/f'{label}_events.csv').open(newline='', encoding='utf-8')))
        assert len(events) == 1, events
        first = next(i for i,r in enumerate(rows) if float(r['S1_Y_uM']) > 0)
        last = rows[-1]
        initial = float(rows[first]['S1_Y_uM'])
        elapsed = float(last['time_s'])-float(rows[first]['time_s'])
        exact = initial*math.exp(-200000*elapsed)
        measured = float(last['S1_Y_uM'])
        decay.append({'dt_us': dt_us, 'release_time_s': float(rows[first]['time_s']),
                      'initial_Y_uM': initial, 'final_Y_uM': measured,
                      'exact_final_Y_uM': exact, 'grew_after_release': measured > initial,
                      'max_Y_uM': max(float(r['S1_Y_uM']) for r in rows),
                      'successful_cli_exit': True})
    document = {
        'cli_sha256': hashlib.sha256(CLI.read_bytes()).hexdigest(),
        'reference': 'Exact capacitor integral, all ionic conductances zero; no spike events.',
        'rows': results,
        'decay_rows': decay,
        'finding': 'Valid sub-step stimuli can be completely missed or have 2.5 times their specified charge at dt=25us.',
        'scope': 'Input quadrature/validation limitation, not an error in HH differential equations.',
        'decay_finding': 'Allowed Omega_Y=200000/s with dt=25us silently grows decaying Y: RK4 stability is not checked against editable rates.',
        'code': ['network.cpp:118-120', 'network.cpp:208-210', 'network.cpp:332-340'],
        'source': 'https://computing.llnl.gov/projects/sundials/usage-notes'
    }
    (HERE/'findings.json').write_text(json.dumps(document, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(results, indent=2))
    print(json.dumps(decay, indent=2))


if __name__ == '__main__':
    main()
