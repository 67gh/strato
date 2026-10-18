#!/usr/bin/env python3
"""Decode Strato native_crash.bin. Host symbols require the exact ELF Build ID.
Usage: python3 tools/symbolize_diagnostic.py SESSION_DIR --symbols UNSTRIPPED_DIR
       --addr2line /path/to/llvm-addr2line --readelf /path/to/llvm-readelf
Guest PCs are not host module offsets and are deliberately left unresolved.
"""
import argparse,json,pathlib,re,shutil,struct,subprocess

def decode(data):
    if not data: return {"status":"no_record"}
    if len(data)<96: return {"status":"partial_record","bytes":len(data)}
    values=struct.unpack('<12Q',data[:96])
    names=('magic','version','signal','tid','ns','fault','pc','lr','sp','fp','guest','reserved')
    result=dict(zip(names,values))
    if result['magic']!=0x3152434f54415253 or result['version']!=1: raise ValueError('Unrecognized native record')
    result['status']='recorded';return result

def build_id(file,readelf):
    run=subprocess.run([readelf,'-n',str(file)],capture_output=True,text=True,timeout=20,check=True)
    match=re.search(r'Build ID:\s*([0-9a-fA-F]+)',run.stdout)
    return match.group(1).lower() if match else ''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session',type=pathlib.Path);parser.add_argument('--symbols',type=pathlib.Path)
    parser.add_argument('--addr2line',default=shutil.which('llvm-addr2line') or shutil.which('addr2line'))
    parser.add_argument('--readelf',default=shutil.which('llvm-readelf') or shutil.which('readelf'))
    args=parser.parse_args();record=decode((args.session/'native_crash.bin').read_bytes())
    record['frames']=[]
    if record.get('status')=='recorded' and not record['guest']:
        modules=json.loads((args.session/'native_modules.json').read_text())['modules']
        for register in ('pc','lr'):
            address=record[register];frame={'register':register,'address':address,'status':'module_not_found'}
            # LR points after the call instruction. Resolve its owning instruction.
            lookup=address-1 if register=='lr' and address else address
            module=next((m for m in modules if m['start']<=lookup<m['end']),None)
            if module:
                frame.update(module=module['path'],build_id=module['build_id'],offset=lookup-module['base'],status='matching_symbols_required')
                if args.symbols and args.readelf and args.addr2line and module['build_id']:
                    for candidate in sorted(args.symbols.rglob(pathlib.Path(module['path']).name)):
                        if not candidate.is_file():continue
                        try:
                            if build_id(candidate,args.readelf)!=module['build_id']:continue
                            result=subprocess.run([args.addr2line,'-f','-C','-i','-e',str(candidate),hex(frame['offset'])],text=True,capture_output=True,timeout=20,check=True)
                            frame.update(status='build_id_matched',symbols=result.stdout.strip(),symbol_file=str(candidate));break
                        except (subprocess.SubprocessError,OSError) as error:frame['symbol_error']=str(error)
            record['frames'].append(frame)
    elif record.get('guest'):
        record['note']='Guest PC/LR: use guest module/loader evidence and hang_snapshot; do not apply host ELF offsets.'
    print(json.dumps(record,indent=2))
if __name__=='__main__':main()
