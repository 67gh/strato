from pathlib import Path
import subprocess,tempfile,json,os,struct,importlib.util,xml.etree.ElementTree as ET,sys
test_source=Path(__file__).resolve().parent;project=test_source.parents[1];out=Path(tempfile.mkdtemp(prefix='strato-diagnostic-tests-'));src=project/'app/src/main/cpp/skyline'
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
results=[]
def run(name,args):
 r=subprocess.run([str(x) for x in args],capture_output=True,text=True,env=env,timeout=90)
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(name+': '+(r.stdout+r.stderr)[-7000:])
 results.append(name+': PASS');return r.stdout
common=['g++','-std=c++20','-pthread','-g','-O1','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(src)]
run('recorder_build',common+[test_source/'host_test.cpp',src/'common/diagnostic_recorder.cpp',src/'common/diagnostic_crash.cpp','-ldl','-o',out/'host_test'])
session=Path(tempfile.mkdtemp(prefix='session-',dir=out))
run('recorder_runtime',[out/'host_test',session])
for file in session.glob('*.json'):json.loads(file.read_text())
events=[json.loads(x) for x in (session/'initial_events.jsonl').read_text().splitlines()]
e=next(e for e in events if e['kind']=='test');assert (e['submission'],e['pipeline'],e['resource'],e['text'])==(7,11,13,'quote" newline\n')
status=json.loads((session/'recorder_status.json').read_text());assert status['draws']==status['dispatches']==status['submissions']==status['completions']==status['presents']==1
assert status['total_events']>8192 and status['overwritten_events']>0
mapping=json.loads((session/'gpu_mappings.json').read_text());assert mapping['dropped_updates']==1
assert [(x['start'],x['end'],x['host'],x['sparse']) for x in mapping['regions']]==[(100,120,1000,False),(130,140,1030,False),(140,160,2000,False),(160,180,0,True),(180,200,1080,False)]
missing=json.loads((session/'unsupported.json').read_text());assert missing['entries'][0]['count']==2
assert len(list(session.glob('shader-*')))==1
assert (session/'native_crash.bin').stat().st_size==96
assert any(m['build_id'] for m in json.loads((session/'native_modules.json').read_text())['modules'])
results.append('JSON, correlation, counters, overlap/sparse/holes, dedup, bounded concurrent ring, first crash, ELF Build IDs: PASS')
vk=project/'app/libraries/vkhpp'
run('vulkan_api_build',common+['-I'+str(vk),'-I'+str(vk/'Vulkan-Headers/include'),'-I'+str(project/'app/libraries/renderdoc'),test_source/'vulkan_api_test.cpp',src/'common/diagnostic_recorder.cpp','-o',out/'vulkan_api_test'])
run('vulkan_api_runtime',[out/'vulkan_api_test',session]);fault=json.loads((session/'device_fault.json').read_text());assert fault['requested_addresses']==999 and fault['query_result']==5 and fault['addresses'][0]['address']==1234
report=json.loads(run('offline_inspector',[sys.executable,project/'tools/inspect_diagnostic.py',session,'--range','9','90','120']))
assert report['mapping_query']['unmapped_ranges']==[[90,100],[120,130],[200,210]]
assert not report['mapping_query']['complete_observation']
crash=json.loads(run('offline_crash',[sys.executable,project/'tools/symbolize_diagnostic.py',session]));assert crash['pc']==7 and crash['guest']==0
for file in [project/'app/src/main/res/layout/emu_activity.xml',project/'app/src/main/res/layout/diagnostic_activity.xml',project/'app/src/main/res/values/diagnostic_tools.xml',project/'app/src/main/res/values-fr/diagnostic_tools.xml']:ET.parse(file)
results.append('XML layouts and strings: PASS')
(out/'RESULTATS.txt').write_text('\n'.join(results)+'\nLIMITES: exécution hôte x86_64 avec ASan/UBSan; Vulkan simulé pour device fault. Pas de compilation APK, pas de driver/GPU/Android exécuté.\n')
print('\n'.join(results));print('Reports:',out)
