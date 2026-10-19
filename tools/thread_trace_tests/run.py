#!/usr/bin/env python3
"""Actual recorder on Linux. No Android/GPU or guest scheduling simulated as validated."""
import pathlib,subprocess,tempfile,os,json
project=pathlib.Path(__file__).resolve().parents[2]
r=project/'app/src/main/cpp/skyline'
output=pathlib.Path(os.environ.get('TRACE_TEST_OUTPUT','/tmp/strato_trace_tests'));output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
results=[]
for variant in ['actual','small_budget']:
 source=r/'common/diagnostic_recorder.cpp'
 if variant=='small_budget':
  source=output/'recorder_small.cpp'
  source.write_text((r/'common/diagnostic_recorder.cpp').read_text().replace('TraceBudget=32*1024*1024','TraceBudget=4096'))
 exe=output/variant
 cmd=['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Wconversion','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(r),'-I'+str(r/'common'),str(source),str(pathlib.Path(__file__).with_name('test.cpp')),'-o',str(exe)]
 subprocess.run(cmd,check=True,env=env)
 for mode in (['normal','off','io','legacy'] if variant=='actual' else ['budget']):
  d=output/(variant+'_'+mode)
  subprocess.run([str(exe),str(d),mode],env=env,check=True,timeout=20)
  status=json.loads((d/'session_trace_status.json').read_text());live=json.loads((d/'live_snapshot_test.json').read_text());final=json.loads((d/'thread_activity.json').read_text())
  assert any(t['guest_id']==5 and any(c['kind']=='svc' and c['arg5']==6 for c in t['active']) for t in live['threads'])
  assert all(not t['active'] for t in final['threads'])
  if mode=='io':assert status['io_failed_events']>0
  else:
   events=[json.loads(l) for l in (d/'session_trace.jsonl').read_text().splitlines()]
   seq=[x['seq'] for x in events];assert seq==sorted(set(seq))
   assert (d/'session_trace.jsonl').stat().st_size<=status['budget_bytes']
   if mode=='off':assert not events and not status['enabled']
   else:assert events[0]['kind']=='session_start'
  if mode=='budget':assert status['budget_drops']>0
  if mode=='normal':assert status['queue_drops']>0
  results.append({'variant':variant,'mode':mode,'passed':True,'status':status})
(output/'RESULTATS.json').write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))

svc=(r/'kernel/svc.cpp').read_text()
assert svc.index('void WaitSynchronization(')<svc.index('"svc_wait_handle"')<svc.index('void CancelSynchronization(')
nce=(r/'nce.cpp').read_text()
assert 'diagnostics::EndActivity(callId' in nce
assert 'diagnostics::Activity ' not in (r/'kernel/scheduler.cpp').read_text()
print('Integration placement checks passed (static, not Android compilation)')
