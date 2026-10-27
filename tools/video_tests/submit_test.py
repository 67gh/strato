#!/usr/bin/env python3
"""Compile the production Submit body with fake nvmap/decoder; test lifetime and fences."""
from pathlib import Path
import subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'app/src/main/cpp/skyline'
s=(SRC/'services/nvdrv/devices/nvhost/host1x_channel.cpp').read_text();a=s.index('    PosixResult Host1xChannel::Submit(');b=s.index('    PosixResult Host1xChannel::GetSyncpoint',a);body=s[a:b]
shim=r'''
#include <soc/host1x/command_parser.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>
using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;using std::span;
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
template<class...T> std::runtime_error exception(const char*s,T...){return std::runtime_error(s);}
enum class PosixResult{Success,InvalidArgument,NotSupported};
inline unsigned done{},reserved{},executions{},pins{},unpins{};
inline bool fail{},relocated{},finished{};
inline std::array<u32,4> storage{0x40000000,0,0,0};
namespace core{
enum class ChannelType {NvDec,VIC,Other};
struct NvMap {
 struct Handle {std::mutex mutex;u64 address=reinterpret_cast<u64>(storage.data()),size=sizeof(storage);bool allocated=true;};
 std::shared_ptr<Handle> h=std::make_shared<Handle>();
 std::shared_ptr<Handle> GetHandle(u32 id){return id==1||id==2?h:nullptr;}
 u32 PinHandle(u32 id){assert(id==2);++pins;return 0x8000;}
 void UnpinHandle(u32 id){assert(id==2);++unpins;}
};
}
struct DeviceState{struct Soc{struct{int syncpoints{};}host1x;};std::shared_ptr<Soc>soc=std::make_shared<Soc>();};
namespace soc::host1x{
using skyline::soc::host1x::ParseCommandBuffer;
class ChannelCommandFifo{public:
 ChannelCommandFifo(const DeviceState&,int&){}
 void Validate(span<u32> words){if(words[0]==0xbad)throw std::runtime_error("bad gather");}
 void SetWaitTimeout(std::chrono::milliseconds){}
 void Process(span<u32> words){++executions;assert(reserved>=done);if(fail)throw std::runtime_error("decode failed");if(relocated){assert(pins==unpins+1);assert(words[1]==0x80);assert(storage[1]==0);}finished=true;done+=2;}
};}
struct Core{core::NvMap nvMap;struct{
 bool IsSyncpointAllocated(u32 id){return id==7;}
 u32 IncrementSyncpointMaxExt(u32 id,u32 amount){assert(id==7);return reserved+=amount;}
 u32 UpdateMin(u32 id){assert(id==7);assert(finished);return done;}
}syncpointManager;};
struct Host1xChannel{
 struct SubmitCmdBuf{u32 mem,offset,words;};struct SubmitReloc{u32 patchMem,patchOffset,pinMem,pinOffset;};struct SubmitSyncpointIncr{u32 syncpointId,numIncrs;std::array<u32,3>res;};
 Core core;DeviceState state;std::mutex channelMutex;core::ChannelType channelType=core::ChannelType::NvDec;
 std::unique_ptr<soc::host1x::ChannelCommandFifo>videoChannel;bool executionFailed{};u32 submitTimeout=5000;
 PosixResult Submit(span<SubmitCmdBuf>,span<SubmitReloc>,span<u32>,span<SubmitSyncpointIncr>,span<u32>);
};
'''
main=r'''
int main(){
 Host1xChannel c;std::array<Host1xChannel::SubmitCmdBuf,1> cmd{{{1,0,4}}};
 std::array<Host1xChannel::SubmitSyncpointIncr,1> inc{{{7,2,{}}}};std::array<u32,1> fence{0xfeed};
 std::array<Host1xChannel::SubmitReloc,1> rel{{{1,4,2,0}}};std::array<u32,1> shifts{8};
 auto invalid=[&]{assert(c.Submit(cmd,{}, {},inc,fence)==PosixResult::InvalidArgument);assert(reserved==0&&done==0&&fence[0]==0xfeed);};
 cmd[0].mem=999;invalid();cmd[0].mem=1;
 cmd[0].offset=2;invalid();cmd[0].offset=16;invalid();cmd[0].offset=0;
 cmd[0].words=0xffffffff;invalid();cmd[0].words=4;
 inc[0].syncpointId=192;invalid();inc[0].syncpointId=7;
 storage[0]=0xbad;invalid();storage[0]=0x40000000;
 shifts[0]=32;assert(c.Submit(cmd,rel,shifts,inc,fence)==PosixResult::InvalidArgument);assert(pins==0);shifts[0]=8;
 rel[0].patchOffset=16;assert(c.Submit(cmd,rel,shifts,inc,fence)==PosixResult::InvalidArgument);rel[0].patchOffset=4;
 relocated=true;assert(c.Submit(cmd,rel,shifts,inc,fence)==PosixResult::Success);assert(done==2&&reserved==2&&fence[0]==2&&pins==unpins);
 fail=true;assert(c.Submit(cmd,rel,shifts,inc,fence)==PosixResult::InvalidArgument);assert(fence[0]==2&&done==2&&reserved==4&&pins==unpins);
 fail=false;const auto n=executions;assert(c.Submit(cmd,{}, {},inc,fence)==PosixResult::InvalidArgument);assert(executions==n);
 std::cout<<"PASS production Submit: validation, owned relocations, pins alive until completion, real completion before fence, decoder failure poisons channel without publishing fence\n";
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp);(p/'test.cpp').write_text(shim+body+main)
 subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-g','-I'+str(SRC),'-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
