#!/usr/bin/env python3
"""Inspect a Host1x H1VC diagnostic snapshot; optionally extract raw chunks.
These packets are emulator inputs, not standalone movies and not decoded frames.
"""
import argparse,json,struct
from pathlib import Path
NAMES={1:'picture',2:'bitstream',3:'vic_config',4:'vp9_probabilities'}

def parse(data):
    if len(data)<32:raise ValueError('truncated H1VC header')
    magic,version,engine,sequence,codec,register_count,chunk_count,status=struct.unpack_from('<8I',data)
    if magic!=0x43563148 or version!=1:raise ValueError('unsupported H1VC format')
    if engine not in (1,2) or register_count!=0x446 or chunk_count>3 or status not in (0,1):raise ValueError('invalid snapshot header')
    end=32+4*register_count
    if len(data)<end:raise ValueError('truncated registers')
    registers=struct.unpack_from('<'+str(register_count)+'I',data,32)
    chunks=[];offset=end
    for _ in range(chunk_count):
        if len(data)-offset<12:raise ValueError('truncated chunk header')
        tag,address,size=struct.unpack_from('<3I',data,offset);offset+=12
        if tag not in NAMES or size>1024*1024 or not size or len(data)-offset<size:raise ValueError('invalid chunk')
        chunks.append({'kind':NAMES[tag],'address':address,'size':size,'data':data[offset:offset+size]});offset+=size
    if offset!=len(data):raise ValueError('unexpected trailing bytes')
    return {'engine':'NVDEC' if engine==1 else 'VIC','sequence':sequence,
            'codec':{3:'H264',5:'VP8',9:'VP9'}.get(codec,str(codec)) if engine==1 else None,
            'incomplete':bool(status),'registers':{hex(i):hex(v) for i,v in enumerate(registers) if v},'chunks':chunks}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('snapshot',type=Path);p.add_argument('--output-dir',type=Path);args=p.parse_args()
    result=parse(args.snapshot.read_bytes())
    if args.output_dir:args.output_dir.mkdir(parents=True,exist_ok=True)
    for i,chunk in enumerate(result['chunks']):
        payload=chunk.pop('data')
        if args.output_dir:(args.output_dir/(str(i)+'-'+chunk['kind']+'.bin')).write_bytes(payload)
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
