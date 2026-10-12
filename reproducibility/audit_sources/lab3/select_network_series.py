"""Select completed results per frequency, preserving explicit numerical provenance.

Prefer the production executable; use FTZ+DAZ only where no completed production
result exists. Never modify either original result collection.
"""
import sys
sys.dont_write_bytecode=True
import audit as a
import network_audit as n

def main():
    production=a.read(a.HERE/'network_findings.json')
    ftz=a.read(a.HERE/'ftz_findings.json')
    times={r['name']:r['wall_s'] for r in production['runs']}
    diagnostics={r['original_name']:r for r in ftz['runs'] if 'network_metrics' in r}
    result=dict(scope=__doc__,imports=production['imports'],runs=[],script_sha256=a.sha(__file__))
    for source in ['net1_0.1Hz.net','net1_1Hz.net','net1_100Hz.net']:
        name='network_'+source.removesuffix('.net').replace('.','p')
        if a.rawpath(name,'summary.json').exists():
            c,info=n.import_network(n.SOURCE/source)
            row=n.analyze(name,c,info,times.get(name))
            row.update(numerical_mode='production default MXCSR',cli_sha256=production['cli_sha256'])
        else:
            d=diagnostics[name]
            row=dict(d['network_metrics'])
            row.update(numerical_mode='diagnostic FTZ+DAZ',cli_sha256=d['cli_sha256'],original_run_unavailable=True)
        result['runs'].append(row)
    result['mixed_numerical_modes']=len({r['numerical_mode'] for r in result['runs']})>1
    a.save(a.HERE/'network_selected_findings.json',result)
    print([(r['frequency_hz'],r['numerical_mode']) for r in result['runs']])

if __name__=='__main__':main()
