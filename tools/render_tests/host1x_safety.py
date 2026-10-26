#!/usr/bin/env python3
"""CPU regression tests. Production parser/Syncpoint sources are compiled directly.
Submit is extracted with mocked nvmap/state; no Android or hardware decoding test.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'app/src/main/cpp/skyline'
ENV={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
COMMON=r'''
#pragma once
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <utility>
using u32=std::uint32_t;
'''
PARSER=r'''
#include <soc/host1x/command_parser.h>
#include <cassert>
#include <vector>
#include <array>
#include <tuple>
#include <random>
#include <iostream>
using namespace skyline::soc::host1x;
using Word=std::uint32_t;
int main(){
 std::vector<std::tuple<unsigned,unsigned,unsigned>> calls;
 auto emit=[&](unsigned c,unsigned m,unsigned a){calls.emplace_back(c,m,a);};
 std::vector<Word> words{0x00003c00,0x4010abcd};
 auto cls=ParseCommandBuffer(words,1,emit);assert(cls==0xf0&&calls.size()==1);
 assert(calls[0]==std::make_tuple(0xf0U,0x10U,0xabcdU));
 calls.clear();cls=ParseCommandBuffer(std::array<Word,1>{0x4020ffff},cls,emit);
 assert(calls[0]==std::make_tuple(0xf0U,0x20U,0xffffU));
 assert(ParseCommandBuffer({},cls,emit)==cls);
 for(auto packet:std::vector<std::vector<Word>>{{0x00000001},{0x10000001},{0x20000002,0},{0x30008001,0},{0x40000001,0x10000001},{0x50000000}}){
  calls.clear();bool rejected=false;try{ParseCommandBuffer(packet,1,emit);}catch(const std::invalid_argument&){rejected=true;}
  assert(rejected&&calls.empty()); // Reject before the first method, including valid prefixes.
 }
 calls.clear();
 ParseCommandBuffer(std::array<Word,4>{0x10010003,10,11,12},1,emit);
 assert(calls.size()==3&&std::get<1>(calls[2])==3);
 calls.clear();ParseCommandBuffer(std::array<Word,3>{0x20010002,10,11},1,emit);
 assert(calls.size()==2&&std::get<1>(calls[1])==1);
 calls.clear();ParseCommandBuffer(std::array<Word,3>{0x30018001,10,11},1,emit);
 assert(calls.size()==2&&std::get<1>(calls[1])==16);
 calls.clear();ParseCommandBuffer(std::array<Word,3>{0x00003c21,10,11},1,emit);
 assert(calls.size()==2&&std::get<1>(calls[1])==5);
 std::mt19937 rng(42);
 for(int n=0;n<50000;++n){std::vector<Word> data(rng()%32);for(auto &w:data)w=rng();try{ParseCommandBuffer(data,1,[](auto,auto,auto){});}catch(const std::invalid_argument&){} }
 std::cout<<"PASS parser: IMM16, class continuity, all supported opcodes, empty/truncated/unknown packets, atomic syntax validation, 50000 fuzz inputs\n";
}
'''
SYNC=r'''
#include <common.h>
#define private public
#include <soc/host1x/syncpoint.h>
#undef private
#include <cassert>
#include <future>
#include <iostream>
#include <thread>
using namespace skyline::soc::host1x;
using namespace std::chrono_literals;
int main(){
 Syncpoint s;
 assert(s.Wait(0,0ms));assert(s.Wait(0,std::chrono::steady_clock::duration::max()));
 for(int i=0;i<1000;++i)assert(!s.Wait(1,0ms));
 assert(s.waiters.empty());
 bool called=false;s.RegisterWaiter(1,[&]{called=true;});
 auto future=std::async(std::launch::async,[&]{return s.Wait(1,2s);});
 s.Increment();assert(future.get()&&called&&s.Wait(1,0ms));
 bool cancelled=false;auto removed=s.RegisterWaiter(2,[&]{cancelled=true;});s.DeregisterWaiter(removed);
 s.Increment();assert(!cancelled);
 for(int n=0;n<300;++n){Syncpoint race;auto f=std::async(std::launch::async,[&]{return race.Wait(1,2s);});race.Increment();assert(f.get());}
 Syncpoint multi;
 std::vector<std::future<bool>> futures;
 for(int i=0;i<8;++i)futures.push_back(std::async(std::launch::async,[&]{return multi.Wait(1,std::chrono::steady_clock::duration::max());}));
 multi.Increment();for(auto &f:futures)assert(f.get());
 std::cout<<"PASS real Syncpoint: already reached, 1000 timeouts, callbacks/cancellation, 300 wait/increment races, 8 infinite waiters\n";
}
'''
def function(text,key):
 a=text.index(key);b=text.index('{',a);i=b+1;depth=1
 while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[a:i]
SUBMIT=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
using u32=uint32_t;using u64=uint64_t;using std::span;
#define LOGD(...) ((void)0)
#define LOGW(...) (++warningCount)
inline unsigned warningCount{},diagnosticCount{};
namespace diagnostics{void Unsupported(const char*,u32,const char*){++diagnosticCount;}}
enum class PosixResult{Success,InvalidArgument,NotSupported};
struct Handle{std::mutex mutex;u64 address=0x1000,size=16;bool allocated=true;};
struct NvMap{std::shared_ptr<Handle> h=std::make_shared<Handle>();std::shared_ptr<Handle> GetHandle(u32 id){return id==1?h:nullptr;}};
struct Core{NvMap nvMap;struct{bool IsSyncpointAllocated(u32 id){return id==7;}}syncpointManager;};
struct Host1xChannel{
 struct SubmitCmdBuf{u32 mem,offset,words;};struct SubmitReloc{u32 a,b,c,d;};struct SubmitSyncpointIncr{u32 syncpointId,numIncrs;std::array<u32,3>res;};
 Core core;std::mutex channelMutex;unsigned channelType{};bool reportedUnsupported{};
 PosixResult Submit(span<SubmitCmdBuf>,span<SubmitReloc>,span<u32>,span<SubmitSyncpointIncr>,span<u32>);
};
'''
SUBMIT_MAIN=r'''
int main(){
 Host1xChannel c;std::array<Host1xChannel::SubmitCmdBuf,1> cmd{{{1,0,4}}};
 std::array<Host1xChannel::SubmitSyncpointIncr,1> inc{{{7,2,{}}}};std::array<u32,1> fence{0xfeed};
 // No state/queue/IncrementSyncpointMaxExt exists in these doubles: a write won't compile.
 for(int i=0;i<100;++i)assert(c.Submit(cmd,{}, {},inc,fence)==PosixResult::NotSupported);
 assert(fence[0]==0xfeed&&warningCount==1&&diagnosticCount==1);
 auto bad=[&]{assert(c.Submit(cmd,{}, {},inc,fence)==PosixResult::InvalidArgument);assert(fence[0]==0xfeed);};
 cmd[0].mem=999;bad();cmd[0].mem=1;
 inc[0].syncpointId=192;bad();inc[0].syncpointId=7;
 cmd[0].offset=17;bad();cmd[0].offset=2;bad();cmd[0].offset=4;bad();
 cmd[0].offset=0;cmd[0].words=0xffffffffU;bad();cmd[0].words=4;
 c.core.nvMap.h->allocated=false;bad();c.core.nvMap.h->allocated=true;
 c.core.nvMap.h->address=0;bad();c.core.nvMap.h->address=0x1001;bad();
 c.core.nvMap.h->address=std::numeric_limits<u64>::max()-3;bad();
 c.core.nvMap.h->address=0x1000;
 std::array<Host1xChannel::SubmitReloc,1> reloc{};std::array<u32,1> shift{};
 assert(c.Submit(cmd,reloc,{},inc,fence)==PosixResult::InvalidArgument);
 assert(c.Submit(cmd,reloc,shift,inc,fence)==PosixResult::NotSupported);
 assert(c.Submit({}, {}, {}, {},fence)==PosixResult::InvalidArgument);
 assert(c.Submit({}, {}, {}, {}, {})==PosixResult::Success);
 assert(fence[0]==0xfeed);
 std::cout<<"PASS Submit body: no fake counters, untouched fences, invalid handles/alignment/size/overflow/IDs/relocations, one warning, empty no-op\n";
}
'''
def run():
 with tempfile.TemporaryDirectory(prefix='host1x-safety-') as d:
  p=Path(d);(p/'common.h').write_text(COMMON)
  (p/'parser.cpp').write_text(PARSER)
  (p/'sync.cpp').write_text(SYNC.replace('#include <thread>','#include <thread>\n#include <vector>'))
  submit=function((SRC/'services/nvdrv/devices/nvhost/host1x_channel.cpp').read_text(),'PosixResult Host1xChannel::Submit(')
  (p/'submit.cpp').write_text(SUBMIT+submit+SUBMIT_MAIN)
  allocated=function((SRC/'services/nvdrv/core/syncpoint_manager.cpp').read_text(),'bool SyncpointManager::IsSyncpointAllocated(')
  (p/'allocated.cpp').write_text("#include <array>\n#include <mutex>\n#include <cassert>\n#include <iostream>\nusing u32=unsigned; struct SyncpointManager {struct Info {bool reserved{};}; std::array<Info,192> syncpoints{}; std::mutex reservationLock;bool IsSyncpointAllocated(u32);};\n"+allocated+"\nint main(){SyncpointManager s;s.syncpoints[191].reserved=true;assert(s.IsSyncpointAllocated(191));assert(!s.IsSyncpointAllocated(192));assert(!s.IsSyncpointAllocated(0xffffffffU));assert(!s.IsSyncpointAllocated(0));std::cout<<\"PASS syncpoint bounds: 0,191,192,UINT_MAX\\n\";}\n")
  push=function((SRC/'soc/host1x/command_fifo.cpp').read_text(),'void ChannelCommandFifo::Push(')
  (p/'ownership.cpp').write_text("#include <vector>\n#include <span>\n#include <cassert>\n#include <iostream>\nusing u32=unsigned;using std::span;struct ChannelCommandFifo {struct Q {std::vector<std::vector<u32>> values;void Push(std::vector<u32> data){values.push_back(std::move(data));}} gatherQueue;void Push(span<u32>);};\n"+push+"\nint main(){ChannelCommandFifo f;{std::vector<u32> input{1,2,3};f.Push(input);input[0]=99;}assert(f.gatherQueue.values[0][0]==1);f.Push({});assert(f.gatherQueue.values.size()==1);std::cout<<\"PASS FIFO ownership: source changed/destroyed after Push, empty gather\\n\";}\n")
  flags=[os.environ.get('CXX','g++'),'-std=c++20','-O1','-g','-pthread','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(p),'-I'+str(SRC)]
  for name in ('parser','sync','submit','allocated','ownership'):
   extra=[SRC/'soc/host1x/syncpoint.cpp'] if name=='sync' else []
   subprocess.run(flags+[str(p/(name+'.cpp'))]+[str(f) for f in extra]+['-o',str(p/name)],check=True)
   subprocess.run([str(p/name)],check=True,timeout=30,env=ENV)
if __name__=='__main__':run()
