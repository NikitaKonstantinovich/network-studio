"""Verify delivered release/evidence and freeze a compact local artifact manifest."""
from pathlib import Path
import datetime
import hashlib
import json
import re

HERE=Path(__file__).resolve().parent
STUDIO=HERE.parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def save(p,data):p.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
cli=STUDIO/'build/Release/network_studio_cli.exe'
gui=STUDIO/'build/Release/network_studio.exe'
current=sha(cli)
assert current=='38e1a6a15f357fee98f0f674656de0e6e0332db68cdccf43267de5185ec8373f'
assert sha(HERE/'pairs/baseline/network_studio_cli.exe')=='4965bb5cc41f13f243bf9823f9c3d6c68c1ab0a77d1677574007003f50342a8e'
updated=load(HERE/'updated/findings_all.json')
assert updated['runs']==36
assert all(r['provenance']['cli_sha256']==current for r in updated['rows'])
network=load(HERE/'release_network_check.json')
assert network['passed'] and network['run_count']==12 and network['byte_identical_csv_count']==36
assert network['cli_sha256_after']==current
for r in updated['rows']:
    for suffix,h in r['provenance']['raw_sha256'].items():
        assert sha(HERE/'updated/raw'/(r['label']+'_'+suffix))==h
    assert sha(HERE/'updated/configs'/(r['label']+'.json'))==r['provenance']['config_sha256']
smoke=load(HERE/'qa/gui/demo_summary.json')
reference=next(r for r in updated['rows'] if r['case']=='t2b_triplet_post_pre_post_1Hz' and r['dt_us']==25)
assert smoke['complete'] and smoke['neuron_spikes']==reference['spike_counts']
assert smoke['final_synapse_weights']==[reference['final_weight']]
assert '100% tests passed, 0 tests failed out of 4' in (HERE/'published_build.log').read_text(encoding='utf-8-sig')
assert 'frames=24' in (HERE/'qa/gui/renderer.txt').read_text()
assert '\nerror=\n' in (HERE/'qa/gui/renderer.txt').read_text()

production=['network.h','network.cpp','io.cpp','gui.cpp','gui.h','tests.cpp','gui_tests.cpp','README.md',
            'CMakeLists.txt','generator.h','generator.cpp','cli.cpp','main.cpp',
            'examples/stdp_triplet.json','examples/stdp_triplet_burst.json']
save(HERE/'provenance.json',dict(checked_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    branch='feature/izhikevich-lab1',scope='Windows CPU standalone Network Studio; not the CUDA executable',
    published_cli=dict(path=str(cli),sha256=current),published_gui=dict(path=str(gui),sha256=sha(gui)),
    core_library_sha256=sha(STUDIO/'build/native/network_core.lib'),
    production_files={p:sha(STUDIO/p) for p in production},
    validation=dict(ctest_suites_passed=4,official_new_lab_runs=28,width1_diagnostic_runs=8,
                    baseline_pair_repeats=16,baseline_network_runs=36,duration_sensitivity_runs=30,
                    published_network_regressions=12,gui_smoke_frames=24,
                    gui_cli_burst_spikes_and_final_weight_equal=True),
    source_reference_sha256=sha(HERE/'sources/source_evidence.json')))

# Compact artifacts/configs are in Git; raw CSVs and binaries have their own
# hashes in findings and remain local. Verify Markdown targets before delivery.
broken=[]
for p in HERE.rglob('*.md'):
    if any(part in {'baseline','baseline_sources','qa'} for part in p.relative_to(HERE).parts):continue
    for target in re.findall(r'\]\(([^)]+)\)',p.read_text(encoding='utf-8-sig')):
        if target.startswith(('https:','http:','#')):continue
        target=target.split('#',1)[0]
        q=Path(target) if re.match(r'^[A-Za-z]:/',target) else p.parent/target
        if not q.exists():broken.append((str(p.relative_to(HERE)),target))
assert not broken,broken
ignored={'qa','raw','__pycache__','baseline_bins'}
files=[]
for p in sorted(HERE.rglob('*')):
    if not p.is_file() or p.name=='manifest.json' or any(s in ignored for s in p.relative_to(HERE).parts):continue
    if p.suffix.lower() in {'.exe','.lib','.obj','.pdb','.pyc'}:continue
    files.append(dict(path=p.relative_to(HERE).as_posix(),bytes=p.stat().st_size,sha256=sha(p)))
save(HERE/'manifest.json',dict(scope='Compact report, scripts, config inputs and findings; raw hashes are in per-series findings.',
    files=files,file_count=len(files),bytes=sum(r['bytes'] for r in files)))
print(f'PASS: 36 new runs, 12 network regressions, GUI/CLI agreement; {len(files)} compact artifacts hashed; Markdown links valid')
