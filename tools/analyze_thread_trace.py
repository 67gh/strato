#!/usr/bin/env python3
"""Summarize native thread evidence without claiming an inferred wait is a deadlock."""
import argparse,json,pathlib,zipfile

def analyze(load):
    snapshot=load('thread_activity.json')
    progress=load('recorder_status.json')
    trace=load('session_trace_status.json')
    result={'cause_proven':False,'progress':progress,'trace_completeness':trace,'threads':[],'requested_owner_cycles':[]}
    if not snapshot:
        h=load('hang_snapshot.json')
        result['limitation']='Older build: no persistent native activity registry.'
        result['guest_pc_samples']=[{'tid':x['tid'],'name':x['name'],'pc':hex(x['pc'])} for x in h.get('samples',[]) if x.get('guest') and 'pc' in x]
        return result
    result['snapshot_quality']={k:snapshot.get(k) for k in ('ns','busy_slots','lost_updates','untracked_updates','paused')}
    by_handle={t['guest_handle']:t for t in snapshot['threads'] if t['guest'] and not t['exited']}
    owners={}
    for t in snapshot['threads']:
        if t['exited']:continue
        active=t['active']
        observation={'tid':t['tid'],'guest_id':t['guest_id'] if t['guest'] else None,'name':t['name'],
                     'host_state':t['host_state'],'host_wchan':t['host_wchan'],
                     'cpu_percent':t['cpu_percent_one_core'] if t['cpu_percent_valid'] else None,
                     'lost_updates':t['lost_updates'],'last_call':t['last_call'],'active':active}
        for c in active:
            if c['kind']=='mutex_lock':
                owner=by_handle.get(c['resource'])
                observation['requested_mutex_owner']={'handle':c['resource'],'guest_id':owner['guest_id'] if owner else None,'address':hex(c['arg0'])}
                if owner and not t['lost_updates']:owners[t['tid']]=owner['tid']
            if c['kind']=='svc' and c['resource'] in (0x18,0x1A,0x1C,0x34):
                observation['wait_svc']=hex(c['resource'])
        if active or (observation['cpu_percent'] or 0)>=50:
            observation['note']='Active native call or CPU activity; normal waits and loading are possible.'
            result['threads'].append(observation)
    cycles=set()
    for start in owners:
        path=[];node=start
        while node in owners and node not in path:
            path.append(node);node=owners[node]
        if node in path:
            cycle=path[path.index(node):];cycles.add(tuple(sorted(cycle)))
    result['requested_owner_cycles']=[{'native_tids':list(c),'certainty':'candidate only: owners are requested at call entry; snapshot is not globally atomic'} for c in sorted(cycles)]
    result['threads'].sort(key=lambda t:max([c['age_ns'] for c in t['active']] or [0]),reverse=True)
    return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('diagnostic',type=pathlib.Path);args=parser.parse_args()
    def decode(data):
        try:return json.loads(data)
        except (ValueError,UnicodeDecodeError):return {}
    if args.diagnostic.is_dir():
        def load(name):
            p=args.diagnostic/name
            return decode(p.read_bytes()) if p.exists() else {}
        result=analyze(load)
    else:
        with zipfile.ZipFile(args.diagnostic) as z:
            def load(name):
                return decode(z.read(name)) if name in z.namelist() else {}
            result=analyze(load)
    print(json.dumps(result,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
