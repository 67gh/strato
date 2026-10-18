#!/usr/bin/env python3
"""Inspect a Strato diagnostic ZIP or directory without extracting it.
Optional --range SPACE ADDRESS SIZE (decimal or 0x...) lists observed GMMU holes.
This is an evidence index, not an automatic proof of a game's root cause.
"""
import argparse,json,pathlib,struct,zipfile

def holes(regions,space,address,size):
    if address<0 or size<0 or address+size>1<<64:raise ValueError('Invalid 64-bit range')
    cursor=address;end=address+size;gaps=[];sparse=[]
    for r in sorted((r for r in regions if r['space']==space),key=lambda r:r['start']):
        a,b=max(address,r['start']),min(end,r['end'])
        if a>=b:continue
        if a>cursor:gaps.append([cursor,a])
        if r['sparse']:sparse.append([a,b])
        cursor=max(cursor,b)
    if cursor<end:gaps.append([cursor,end])
    return {'unmapped_ranges':gaps,'sparse_ranges':sparse}

class Evidence:
    def __init__(self,path):
        self.path=path;self.archive=zipfile.ZipFile(path) if path.is_file() else None
    def names(self):return self.archive.namelist() if self.archive else [p.name for p in self.path.iterdir() if p.is_file()]
    def read(self,name):
        if '/' in name or '\\' in name or name in ('.','..'):raise ValueError('Only flat session files')
        if self.archive:
            if self.archive.getinfo(name).file_size>24*1024*1024:raise ValueError('Evidence too large')
            return self.archive.read(name)
        file=self.path/name
        if file.stat().st_size>24*1024*1024:raise ValueError('Evidence too large')
        return file.read_bytes()
    def json(self,name):
        try:return json.loads(self.read(name))
        except (KeyError,FileNotFoundError):return {'status':'missing'}

def inspect(evidence,query=None):
    status=evidence.json('recorder_status.json');features=evidence.json('diagnostic_features.json')
    report={'progress':status,'features':features,'hang':evidence.json('hang_snapshot.json'),
        'unsupported':evidence.json('unsupported.json'),'device_fault':evidence.json('device_fault.json'),
        'interpretation':'Counters are attempts or observed completions. Missing/dropped evidence and unsupported tools prevent a complete diagnosis.'}
    mappings=evidence.json('gpu_mappings.json')
    if query:
        report['mapping_query']=holes(mappings.get('regions',[]),*query)
        report['mapping_query']['complete_observation']=mappings.get('dropped_updates',1)==0 and 'regions' in mappings
    artifacts=[]
    for name in evidence.names():
        if name.startswith('shader-'):
            entry={'file':name}
            if name.endswith('.spv'):
                data=evidence.read(name);entry['spirv_header_valid']=len(data)>=20 and len(data)%4==0 and struct.unpack('<I',data[:4])[0]==0x07230203
            artifacts.append(entry)
    report['artifacts']=artifacts
    try:
        events=[json.loads(line) for line in evidence.read('flight_recorder.jsonl').splitlines() if line.strip()]
        report['mapping_failures']=[e for e in events if e['kind'] in ('buffer_unmapped','buffer_partial_mapping')]
        report['pipelines_and_shaders']=[e for e in events if e['kind'] in ('pipeline_shader','pipeline_from_cache','graphics_pipeline','compute_pipeline')]
        report['last_buffer_writes']={}
        for e in events:
            if e['kind'] in ('buffer_write_attempt','buffer_cpu_write_trap','buffer_gpu_write_scheduled'):
                report['last_buffer_writes'][str(e['resource'])+':'+e['kind']]=e
        report['resource_history_scope']='Only retained ring events; GPU writes marked scheduled are not proof of completion'
    except (KeyError,FileNotFoundError):report['events_status']='missing'
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('session',type=pathlib.Path)
    parser.add_argument('--range',dest='query',nargs=3,type=lambda s:int(s,0),metavar=('SPACE','ADDRESS','SIZE'))
    args=parser.parse_args();print(json.dumps(inspect(Evidence(args.session),args.query),indent=2))
if __name__=='__main__':main()
