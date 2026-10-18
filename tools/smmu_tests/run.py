#!/usr/bin/env python3
"""Compile production SMMU + mapping templates on a host with ASan/UBSan.
Android process/diagnostic dependencies are stubbed; no video/GPU execution.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'app/src/main/cpp/skyline'
def clean(s):
    return '\n'.join(line for line in s.splitlines() if not line.startswith('#include') and line!='#pragma once')
PREFIX='\n#include <algorithm>\n#include <bit>\n#include <cstdint>\nusing u32=uint32_t;\n#include <array>\n#include <cassert>\n#include <cstdint>\n#include <cstring>\n#include <cerrno>\n#include <functional>\n#include <iostream>\n#include <limits>\n#include <memory>\n#include <mutex>\n#include <optional>\n#include <random>\n#include <shared_mutex>\n#include <span>\n#include <stdexcept>\n#include <type_traits>\n#include <utility>\n#include <vector>\n#include <sys/mman.h>\nusing u8=uint8_t;using u64=uint64_t;\n#define TRACE_EVENT(...)\nnamespace boost::container {template<class T,size_t N>using small_vector=std::vector<T>;}\nnamespace skyline {\nnamespace diagnostics {using Id=uint64_t;inline Id NewId(){return 1;}inline void DestroyAddressSpace(Id){}inline void Mapping(Id,uint64_t,uint64_t,uintptr_t,bool,bool){}}\nusing SharedSpinLock=std::shared_mutex;\nstruct exception:std::runtime_error {template<class...T> exception(const char*s,T...):std::runtime_error(s){}};\nnamespace util {\ntemplate<class T>concept TrivialObject=std::is_trivial_v<T>;\nconstexpr size_t DivideCeil(size_t a,size_t b){return (a+b-1)/b;}\nconstexpr size_t AlignUp(size_t a,size_t b){return (a+b-1)&~(b-1);}\n}\ntemplate<class T,size_t E=std::dynamic_extent>struct span:std::span<T,E>{\nusing std::span<T,E>::span;\nspan(const std::span<T,E>&s):std::span<T,E>(s){}\nbool valid()const{return this->data()!=nullptr;}\nspan<T> subspan(size_t o,size_t n=std::dynamic_extent)const{return std::span<T,E>::subspan(o,n);}\nspan<T> first(size_t n)const{return std::span<T,E>::first(n);}\ntemplate<class U,size_t F>void copy_from(span<U,F>s){std::memcpy(this->data(),s.data(),s.size_bytes());}\n};\ntemplate<class T>span(T*,size_t)->span<T>;\n}\n'
TEST=r"""
using namespace skyline;
int main(){
    constexpr u64 end=1ULL<<32;
    std::vector<u8> data(0x8000,0x5a);
    soc::SMMU smmu;
    assert(smmu.vaLimit==end);
    // The exact address and size in Hollow Knight's fatal exception.
    smmu.Map(0x1000,data.data(),0x8000);
    auto spans=smmu.TranslateRange(0x1000,0x8000);
    assert(spans.size()==1 && spans[0].data()==data.data() && spans[0].size()==0x8000);
    assert(smmu.Read<u8>(0x1000)==0x5a);
    smmu.Unmap(0x3000,0x1000);
    assert(!smmu.TranslateRange(0x3000,1)[0].valid());
    assert(smmu.TranslateRange(0x4000,1)[0].data()==data.data()+0x3000);
    smmu.Map(0x3000,data.data()+0x2000,0x1000);
    // Preserve all 32 guest address bits, including the final byte.
    smmu.Map(end-0x1000,data.data(),0x1000);
    assert(smmu.TranslateRange(end-1,1)[0].data()==data.data()+0xfff);
    assert(smmu.Read<u8>(end-1)==0x5a);
    smmu.Write(end-1,static_cast<u8>(0x7b));
    assert(data[0xfff]==0x7b);
    smmu.Unmap(end-0x1000,0x1000);
    assert(!smmu.TranslateRange(end-1,1)[0].valid());
    bool rejected=false;try{smmu.Map(end-1,data.data(),2);}catch(const std::exception&){rejected=true;}
    assert(rejected);
    rejected=false;try{smmu.TranslateRange(end,1);}catch(const std::exception&){rejected=true;}
    assert(rejected);
    // FlatAllocator still uses guest u32 addresses, independently of the SMMU.
    FlatAllocator<u32,0,32> allocator(0x1000);
    assert(allocator.Allocate(0x8000)==0x1000);
    allocator.Free(0x1000,0x8000);
    // Existing 40-bit GMMU representation must keep its full exclusive limit.
    FlatMemoryManager<u64,0,40,12,17> gmmu;
    assert(gmmu.vaLimit==(1ULL<<40));
    gmmu.Map((1ULL<<40)-0x1000,data.data(),0x1000);
    assert(gmmu.TranslateRange((1ULL<<40)-1,1)[0].data()==data.data()+0xfff);
    std::cout<<"PASS: Hollow Knight range, holes/remap, final SMMU byte, overflow rejection, guest allocator, final GMMU byte\n";
}
"""
def main():
    source=PREFIX+'\n'+clean((SRC/'common/segment_table.h').read_text())+'\n'
    source+=clean((SRC/'common/address_space.h').read_text())+'\n'
    source+=clean((SRC/'common/address_space.inc').read_text())+'\n'
    source+=clean((SRC/'soc/smmu.h').read_text())+'\n'
    source+=clean((SRC/'soc/smmu.cpp').read_text())+'\n'+TEST
    with tempfile.TemporaryDirectory(prefix='strato-smmu-') as directory:
        path=Path(directory); cpp=path/'test.cpp';exe=path/'test'
        cpp.write_text(source)
        flags=[os.environ.get('CXX','g++'),'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
        subprocess.run(flags+[str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
        # A narrow internal address type must now fail during compilation.
        bad=path/'narrow.cpp';bad.write_text(source+'\nvoid narrow(){skyline::FlatMemoryManager<u32,0,32,12,17> invalid;}\n')
        result=subprocess.run(flags+['-fsyntax-only',str(bad)],capture_output=True,text=True)
        assert result.returncode!=0 and 'wide enough for its exclusive end' in result.stderr
        print('PASS: u32/32 invalid specialization rejected at compile time')
        # Reproduce the former truncation from the original constructor semantics.
        previous=source.replace('static_assert(AddressSpaceBits < sizeof(VaType) * 8,\n                      "FlatMemoryManager needs a VA type wide enough for its exclusive end");','')
        previous=previous.replace('using SMMU = FlatMemoryManager<u64,','using SMMU = FlatMemoryManager<u32,')
        previous=previous[:previous.index('using namespace skyline;\nint main()')]+r"""
int main(){skyline::soc::SMMU s;assert(s.vaLimit==0);std::vector<u8> b(0x8000);bool rejected=false;try{s.Map(0x1000,b.data(),0x8000);}catch(const std::exception&){rejected=true;}assert(rejected);std::cout<<"PASS: former u32 limit reproduces Hollow Knight's rejected mapping\n";}
"""
        cpp.write_text(previous)
        subprocess.run(flags+[str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
if __name__=='__main__':main()
