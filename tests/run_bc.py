from pathlib import Path
import subprocess,os
r=Path(__file__).resolve().parents[1];src=r/'app/src/main/cpp/skyline/gpu/texture';out=r/'results';out.mkdir(exist_ok=True)
# Compile the existing decoder implementation, replacing only its platform-wide includes.
decoder=src/'bc_decoder.cpp'
if not decoder.exists():decoder=r/'tests/fixtures/bc_decoder.cpp'
s=decoder.read_text().replace('#include <fmt/printf.h>','#include <array>\n#include <cstdint>\n#include <cstddef>\n#include <algorithm>\n#include <bit>\n#include <cstring>').replace('#include <common.h>','')
(out/'bc_decoder_host.cpp').write_text(s)
cmd=['g++','-std=c++20','-DNDEBUG','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-Wno-attributes','-I'+str(src),'-c',str(out/'bc_decoder_host.cpp'),'-o',str(out/'decoder.o')]
p=subprocess.run(cmd,capture_output=True,text=True);(out/'decoder_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run(['g++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-Wall','-Wextra','-Wconversion','-I'+str(src),str(r/'tests/test_bc.cpp'),str(out/'decoder.o'),'-o',str(out/'test_bc')],capture_output=True,text=True);(out/'bc_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'test_bc')],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'));(out/'bc.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0
