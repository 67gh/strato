import os
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1];out=r/'results';out.mkdir(exist_ok=True)
s=(r/'app/src/main/cpp/skyline/gpu/memory_manager.cpp').read_text()
a=s.index('        try {',s.index('MemoryManager::AllocateStagingBuffer'));b=s.index('\n    Buffer MemoryManager::AllocateBuffer',a)
body=s[a:b]
pre=r'''
#include <memory>
#include <cassert>
#include <iostream>
using u8=unsigned char;
int destroyed{},fail{};
void vmaDestroyBuffer(int,int,int){++destroyed;}
namespace memory {struct StagingBuffer{StagingBuffer(u8*,size_t,int,int,int){}~StagingBuffer(){vmaDestroyBuffer(1,2,3);}};}
template<class T,class...A>std::shared_ptr<T> testMakeShared(A&&...a){if(fail)throw std::bad_alloc();return std::make_shared<T>(std::forward<A>(a)...);}
std::shared_ptr<memory::StagingBuffer> allocate(){
 int vmaAllocator=1,buffer=2,allocation=3;size_t size=64;u8 bytes[64];struct {void*pMappedData;}allocationInfo{bytes};
'''
main=r'''
int main(){for(fail=0;fail<2;fail++){destroyed=0;bool threw=false;try{auto p=allocate();assert(destroyed==0);}catch(const std::bad_alloc&){threw=true;}assert(threw==bool(fail));assert(destroyed==1);}std::cout<<"Staging: successful owner and failed shared allocation each destroy the VMA buffer exactly once OK\n";}
'''
(out/'staging.cpp').write_text(pre+body.replace('std::make_shared<','testMakeShared<')+main)
p=subprocess.run(['g++','-std=c++20','-fsanitize=address,undefined','-no-pie',str(out/'staging.cpp'),'-o',str(out/'staging')],capture_output=True,text=True);(out/'staging_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'staging')],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'));(out/'staging.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0
