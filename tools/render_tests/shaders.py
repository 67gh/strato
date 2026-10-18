#!/usr/bin/env python3
"""Compile the actual SPIR-V emitter and Sirit, then check produced modules.
Usage: python tools/render_tests/shaders.py [--dependencies /path/to/libraries]
Dependencies must contain fmt, sirit (with SPIRV-Headers), boost and range.
Use the commits pinned by this project's submodules. No Android SDK needed.
"""
from pathlib import Path
import argparse, concurrent.futures, os, shutil, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependencies',type=Path,default=ROOT/'app/libraries')
    parser.add_argument('--output',type=Path,help='Keep generated SPIR-V modules in this directory')
    args=parser.parse_args();deps=args.dependencies.resolve()
    compiler=ROOT/'app/libraries/shader-compiler'
    sources=[compiler/'backend/spirv'/f for f in ['spirv_emit_context.cpp','emit_spirv_image.cpp','emit_spirv_image_atomic.cpp']]
    sources += list((compiler/'frontend/ir').glob('*.cpp'))
    sources += list((deps/'sirit/src').glob('*.cpp'))+list((deps/'sirit/src/instructions').glob('*.cpp'))
    sources += [Path(__file__).with_suffix('.cpp')]
    with tempfile.TemporaryDirectory(prefix='strato-shaders-') as directory:
        tmp=Path(directory);(tmp/'shader_compiler').symlink_to(compiler,target_is_directory=True)
        includes=[tmp,deps/'sirit/include',deps/'sirit/src',deps/'sirit/externals/SPIRV-Headers/include',deps/'fmt/include',deps/'range/include',deps/'boost']
        includes+=list((deps/'boost/libs').glob('*/include'))
        cxx=os.environ.get('CXX','g++')
        flags=[cxx,'-std=c++20','-O0','-g','-ffunction-sections','-fdata-sections','-DFMT_HEADER_ONLY','-include','utility']
        if 'g++' in Path(cxx).name:flags+=['-Wno-changes-meaning'] # Old range-v3 vs GCC 13; not production source changes.
        flags+=['-I'+str(p) for p in includes]
        def build(item):
            i,source=item;obj=tmp/(str(i)+'.o')
            result=subprocess.run(flags+['-c',str(source),'-o',str(obj)],capture_output=True,text=True)
            if result.returncode:raise RuntimeError(str(source)+'\n'+result.stderr)
            return obj
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            objects=list(pool.map(build,enumerate(sources)))
        executable=tmp/'shaders'
        subprocess.run([cxx,'-Wl,--gc-sections']+[str(p) for p in objects]+['-o',str(executable)],check=True)
        subprocess.run([str(executable),str(tmp)],check=True,timeout=30)
        validator=shutil.which('spirv-val')
        if validator:
            for module in tmp.glob('*.spv'):
                subprocess.run([validator,'--target-env','vulkan1.1',str(module)],check=True)
            print('PASS: spirv-val Vulkan 1.1')
        else:print('NOTE: spirv-val unavailable; binary instruction checks only, no GPU validation')
        if args.output:
            args.output.mkdir(parents=True,exist_ok=True)
            for module in tmp.glob('*.spv'):shutil.copy2(module,args.output/module.name)
if __name__=='__main__':main()
