#!/usr/bin/env python3
"""Compile production NVDEC/VIC/THI adapters with RAM-backed SMMU, using real decoder."""
from pathlib import Path
import subprocess,tempfile,sys,os
root=Path(__file__).resolve().parents[2];src=root/'app/src/main/cpp/skyline';build=Path(sys.argv[1]);ff=Path(sys.argv[2])
common=r'''
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>
using u8=std::uint8_t;using u16=std::uint16_t;using u32=std::uint32_t;using u64=std::uint64_t;
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
namespace skyline {
struct exception:std::runtime_error{template<class...T>exception(const char *s,T...):std::runtime_error(s){}};
struct DeviceState;
}
'''
soc=r'''
#pragma once
#include <common.h>
namespace skyline {
struct FakeRange{u8*ptr;bool valid()const{return ptr;}};
struct Memory{std::vector<u8>ram=std::vector<u8>(4*1024*1024);unsigned writes{};
 auto TranslateRange(u64 a,size_t n){return std::vector<FakeRange>{{a<ram.size()&&n<=ram.size()-a?ram.data()+a:nullptr}};}
 void Read(u8*p,u64 a,u64 n){std::copy_n(ram.data()+a,n,p);}
 void Write(u64 a,u8*p,u64 n){++writes;std::copy_n(p,n,ram.data()+a);}
};
struct SOC{Memory smmu;};struct DeviceState{std::shared_ptr<SOC>soc=std::make_shared<SOC>();};
}
'''
capture=r'''
#pragma once
#include <common.h>
namespace skyline::soc::host1x{struct VideoCapture{
 enum class Engine{Nvdec,Vic};static constexpr unsigned ExecuteMethod=0xc0;
 explicit VideoCapture(Engine){}
 template<class F>void Method(u32,u32,F){}
};}
'''
main=r'''
#include <soc.h>
#include <soc/host1x/classes/nvdec.h>
#include <soc/host1x/classes/vic.h>
#include <soc/host1x/tegra_host_interface.h>
#include <cassert>
#include <fstream>
#include <iostream>
using namespace skyline;using namespace skyline::soc::host1x;
int main(int argc,char **argv){assert(argc==2);DeviceState state;SyncpointSet sync;
 TegraHostInterface<NvDecClass>nv(sync,state);
 auto method=[&](unsigned m,unsigned v){nv.CallMethod(0x10,m);nv.CallMethod(0x11,v);};
 std::ifstream f(argv[1],std::ios::binary);assert(f);u32 header[7]{};f.read(reinterpret_cast<char*>(header),sizeof(header));
 auto&ram=state.soc->smmu.ram;f.read(reinterpret_cast<char*>(ram.data()+0x1000),header[4]);f.read(reinterpret_cast<char*>(ram.data()+0x4000),header[5]);
 method(0x80,header[0]);method(0x101,0x10);method(0x102,0x40);method(0x10c,0x200);method(0x11d,0x400);
 bool callback=false;sync[7].host.RegisterWaiter(1,[&]{assert(state.soc->smmu.writes==2);callback=true;});
 method(0xc0,1);assert(sync[7].host.Load()==0);nv.CallMethod(0,0x107);assert(callback&&sync[7].guest.Load()==1);
 method(0x80,9);bool failed=false;try{method(0xc0,1);}catch(const std::exception&){failed=true;}assert(failed&&sync[7].host.Load()==1);
 bool vicDone=false;VicClass vic([&]{vicDone=true;},state);failed=false;
 try{vic.CallMethod(0xc0,1);}catch(const std::exception&){failed=true;}assert(failed&&!vicDone);
 std::cout<<"PASS real NVDEC/VIC adapters + THI: surface writes before OpDone, no completion callback on failure (capture mocked)\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'soc/host1x').mkdir(parents=True)
 (p/'common.h').write_text(common);(p/'soc.h').write_text(soc);(p/'soc/host1x/video_capture.h').write_text(capture);(p/'test.cpp').write_text(main)
 files=[p/'test.cpp',src/'soc/host1x/classes/nvdec.cpp',src/'soc/host1x/classes/vic.cpp',src/'soc/host1x/syncpoint.cpp',src/'soc/host1x/video/decoder.cpp',src/'soc/host1x/video/compositor.cpp']
 subprocess.run(['g++','-std=c++20','-g','-fsanitize=address,undefined','-I'+str(p),'-I'+str(src),'-isystem',str(ff/'include'),*[str(x)for x in files],str(ff/'lib/libavcodec.a'),str(ff/'lib/libavutil.a'),'-pthread','-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(build/'h264.jobs')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
