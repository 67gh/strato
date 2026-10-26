#!/usr/bin/env python3
"""Host test of real video snapshots, NVDEC/VIC hooks and THI constructors.
SMMU/logging/syncpoints are doubles; no decoding or video rendering is tested.
"""
from pathlib import Path
import importlib.util,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'app/src/main/cpp/skyline'
COMMON=r"""
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;
inline unsigned warnings{};
#define LOGW(...) (++warnings)
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
namespace skyline {
struct exception : std::runtime_error {template<typename... T> exception(const char *text, T...):std::runtime_error(text){}};
struct Memory{
 std::vector<u8> bytes=std::vector<u8>(0x10000);int reads=0;
 void Read(u8*dst,u64 address,u32 size){++reads;if(address>bytes.size()||size>bytes.size()-address)throw std::runtime_error("unmapped input");std::memcpy(dst,bytes.data()+address,size);}
 void word(size_t offset,u32 value){for(unsigned i=0;i<4;++i)bytes.at(offset+i)=u8(value>>(i*8));}
};
struct SOC{Memory smmu;};struct DeviceState{std::shared_ptr<SOC>soc=std::make_shared<SOC>();};
}
"""
TEST=r"""
#include <common.h>
#include <common/diagnostic_recorder.h>
#include <soc/host1x/classes/nvdec.h>
#include <soc/host1x/classes/vic.h>
#include <soc/host1x/classes/class.h>
namespace skyline::diagnostics {
 bool enabled=true;Id request=0;int unsupported=0,incomplete=0;
 std::vector<std::vector<u8>> saved;
 bool ShaderCaptureEnabled() noexcept{return enabled;}
 Id CaptureRequest() noexcept{return request;}
 void Unsupported(std::string_view,Id,std::string_view) noexcept{++unsupported;}
 void Event(std::string_view kind,Id,Id,Id,std::string_view) noexcept{if(kind=="video_capture_incomplete")++incomplete;}
 void SaveArtifact(std::string_view,Id,const void*b,std::size_t n,std::string_view) noexcept{auto p=static_cast<const u8*>(b);saved.emplace_back(p,p+n);}
}
namespace skyline::soc::host1x {
 struct TestSyncpoint{int count=0;void Increment(){++count;}};
 using SyncpointSet=std::array<TestSyncpoint,192>;
}
"""
MAIN=r"""
struct CompletingBackend {
 std::function<void()> done;
 CompletingBackend(std::function<void()> done,const skyline::DeviceState&):done(std::move(done)){}
 void CallMethod(u32,u32){done();}
};
int main(int argc,char**argv){
 using namespace skyline;using namespace skyline::soc::host1x;using namespace skyline::diagnostics;
 assert(argc==2);DeviceState state;auto&mem=state.soc->smmu;SyncpointSet sync;
 TegraHostInterface<CompletingBackend> completed(sync,state);
 completed.CallMethod(0,8|(1U<<8));assert(sync[8].count==0);
 completed.CallMethod(0x10,1);completed.CallMethod(0x11,0);assert(sync[8].count==1);
 for(u32 arg: {192U,7U|(2U<<8),7U|(3U<<8)}){bool bad=false;try{completed.CallMethod(0,arg);}catch(const exception&){bad=true;}assert(bad);}
 TegraHostInterface<NvDecClass> nv(sync,state);
 auto nvMethod=[&](u32 m,u32 v){nv.CallMethod(0x10,m);bool rejected=false;try{nv.CallMethod(0x11,v);}catch(const exception&){rejected=true;}assert(rejected==(m==VideoCapture::ExecuteMethod));};
 mem.word(0x1048,4);mem.word(0x4000,0x12345678);
 nvMethod(0x80,3);nvMethod(0x101,0x10);nvMethod(0x102,0x40);
 enabled=false;nvMethod(0xc0,1);assert(saved.empty()&&mem.reads==0);
 enabled=true;nvMethod(0xc0,1);nvMethod(0xc0,1);assert(saved.size()==2&&mem.reads==4);
 for(int i=0;i<100;++i)nvMethod(0xc0,1);assert(saved.size()==2&&mem.reads==4&&warnings==1);
 ++request;nvMethod(0xc0,1);assert(saved.size()==3);
 for(int i=0;i<20;++i){++request;nvMethod(0xc0,1);}assert(saved.size()==8);
 nv.CallMethod(0,7);assert(sync[7].count==1); // Existing immediate syncpoint behavior.
 nv.CallMethod(0,7|(1U<<8));assert(sync[7].count==1); // OpDone must not fabricate completion.
 nvMethod(0xc0,1);assert(sync[7].count==1); // Unsupported execution must not release it.

 auto read=[&](u8*p,u64 address,u32 size){mem.Read(p,address,size);};
 VideoCapture vp8(VideoCapture::Engine::Nvdec);vp8.Method(0x80,5,read);vp8.Method(0x101,0x10,read);vp8.Method(0x102,0x40,read);vp8.Method(0xc0,1,read);
 VideoCapture vp9(VideoCapture::Engine::Nvdec);mem.word(0x1030,4);vp9.Method(0x80,9,read);vp9.Method(0x101,0x10,read);vp9.Method(0x102,0x40,read);vp9.Method(0x170,0x50,read);vp9.Method(0xc0,1,read);
 TegraHostInterface<VicClass> vic(sync,state);vic.CallMethod(0x10,0x1c2);vic.CallMethod(0x11,0x60);vic.CallMethod(0x10,0xc0);bool vicRejected=false;try{vic.CallMethod(0x11,1);}catch(const exception&){vicRejected=true;}assert(vicRejected);
 assert(warnings==2&&saved.size()==11);
 VideoCapture bad(VideoCapture::Engine::Nvdec);bad.Method(0x80,3,read);bad.Method(0x101,0x10,read);bad.Method(0x102,0x1000,read);bad.Method(0xc0,1,read);assert(incomplete==1);
 auto before=saved.size();bad.Method(0x10000,0,read);assert(saved.size()==before);
 VideoCapture huge(VideoCapture::Engine::Nvdec);mem.word(0x1048,1024*1024+1);huge.Method(0x80,3,read);huge.Method(0x101,0x10,read);huge.Method(0x102,0x40,read);huge.Method(0xc0,1,read);assert(incomplete==2);
 for(size_t i=0;i<saved.size();++i){std::ofstream f(std::string(argv[1])+"/"+std::to_string(i)+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(saved[i].data()),saved[i].size());}
}
"""
def main():
 spec=importlib.util.spec_from_file_location('capture',ROOT/'tools/inspect_video_capture.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
 with tempfile.TemporaryDirectory(prefix='strato-video-') as directory:
  p=Path(directory);(p/'common.h').write_text(COMMON);(p/'soc.h').write_text('#include <common.h>\n')
  thi='\n'.join(line for line in (SRC/'soc/host1x/tegra_host_interface.h').read_text().splitlines() if not line.startswith('#include') and line!='#pragma once')
  (p/'test.cpp').write_text(TEST+thi+MAIN)
  flags=[os.environ.get('CXX','g++'),'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(p),'-I'+str(SRC)]
  files=[p/'test.cpp',SRC/'soc/host1x/classes/nvdec.cpp',SRC/'soc/host1x/classes/vic.cpp']
  subprocess.run(flags+[str(f) for f in files]+['-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test'),str(p)],check=True,timeout=20,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  captures=[module.parse((p/(str(i)+'.bin')).read_bytes()) for i in range(13)]
  assert captures[0]['codec']=='H264' and captures[0]['chunks'][1]['data']==bytes.fromhex('78563412')
  assert captures[8]['codec']=='VP8' and captures[9]['codec']=='VP9' and captures[9]['chunks'][2]['size']==0xea0
  assert captures[10]['engine']=='VIC' and captures[10]['chunks'][0]['size']==0x610
  assert captures[11]['incomplete'] and captures[12]['incomplete'] and len(captures[11]['chunks'])==1
  good=(p/'0.bin').read_bytes()
  for broken in [b'',good[:31],good[:-1],good+b'X',b'BAD!'+good[4:]]:
   try:module.parse(broken)
   except ValueError:pass
   else:raise AssertionError('malformed snapshot accepted')
 print('PASS: real video hooks/THI, unsupported execution rejected without fake completion, H264/VP8/VP9/VIC capture, bounds, disabled mode, quotas, one warning per engine, binary parser (ASan/UBSan)')
if __name__=='__main__':main()
