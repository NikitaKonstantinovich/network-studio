"""Recheck all replayed population metrics with the stdlib-only implementation.

The saved configs remain in lab2/configs. Outputs are read here. Schema 3->4
default-only additions are normalized for the semantic snapshot comparison;
all provenance SHA checks continue to read the original bytes.
"""
import importlib.util, json, sys
from pathlib import Path
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
STUDIO=HERE.parents[2]
def module(name):
    spec=importlib.util.spec_from_file_location('check_'+name,STUDIO/'lab2'/f'{name}.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod
checker=module('check_metrics');pops=module('populations')
original_read=checker.read_json
def normalized(path):
    value=original_read(path)
    return pops.canonical_config(value) if isinstance(value,dict) and 'schema' in value and 'neurons' in value else value
checker.read_json=normalized
class RedirectRoot:
    def __truediv__(self,key):
        return {'configs/populations':STUDIO/'lab2/configs/populations','results/populations':HERE/'populations'}[key]
manifest=original_read(STUDIO/'lab2/results/populations/manifest.json')
metrics=original_read(HERE/'populations/metrics.json')
provenance=original_read(HERE/'populations/run_provenance.json')
graphs=original_read(STUDIO/'lab2/results/populations/graphs.json')
result=dict(passed=False,self_test=checker.self_test(),schema_normalization='Source canonical_config adds schema4 recording defaults and inhibitory=false; byte provenance checks remain strict.',graphs={},runs={})
for name in sorted({s['graph'] for s in manifest['scenarios'].values()}):
    result['graphs'][name]=checker.graph_check(STUDIO/f'lab2/configs/populations/{name}_edges.csv',graphs[name])
for name,spec in manifest['scenarios'].items():
    result['runs'][name]=checker.run_check(name,spec,RedirectRoot(),provenance[name],metrics['runs'][name])
    print(name,'independent PASS',flush=True)
result['passed']=True
result['source_sha256']={name:checker.digest(STUDIO/'lab2'/f'{name}.py') for name in ['check_metrics','populations']}
(HERE/'independent_metrics.json').write_text(json.dumps(result,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
print('PASS:',len(result['graphs']),'graphs;',len(result['runs']),'runs',flush=True)
