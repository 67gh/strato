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
def run():
 with tempfile.TemporaryDirectory(prefix='host1x-safety-') as d:
  p=Path(d);(p/'common.h').write_text(COMMON)
  (p/'parser.cpp').write_text(PARSER)
  (p/'sync.cpp').write_text(SYNC.replace('#include <thread>','#include <thread>\n#include <vector>'))
  allocated=function((SRC/'services/nvdrv/core/syncpoint_manager.cpp').read_text(),'bool SyncpointManager::IsSyncpointAllocated(')
  (p/'allocated.cpp').write_text("#include <array>\n#include <mutex>\n#include <cassert>\n#include <iostream>\nusing u32=unsigned; struct SyncpointManager {struct Info {bool reserved{};}; std::array<Info,192> syncpoints{}; std::mutex reservationLock;bool IsSyncpointAllocated(u32);};\n"+allocated+"\nint main(){SyncpointManager s;s.syncpoints[191].reserved=true;assert(s.IsSyncpointAllocated(191));assert(!s.IsSyncpointAllocated(192));assert(!s.IsSyncpointAllocated(0xffffffffU));assert(!s.IsSyncpointAllocated(0));std::cout<<\"PASS syncpoint bounds: 0,191,192,UINT_MAX\\n\";}\n")
  push=function((SRC/'soc/host1x/command_fifo.cpp').read_text(),'void ChannelCommandFifo::Push(')
  (p/'ownership.cpp').write_text("#include <vector>\n#include <span>\n#include <cassert>\n#include <iostream>\nusing u32=unsigned;using std::span;struct ChannelCommandFifo {struct Q {std::vector<std::vector<u32>> values;void Push(std::vector<u32> data){values.push_back(std::move(data));}} gatherQueue;void Push(span<u32>);};\n"+push+"\nint main(){ChannelCommandFifo f;{std::vector<u32> input{1,2,3};f.Push(input);input[0]=99;}assert(f.gatherQueue.values[0][0]==1);f.Push({});assert(f.gatherQueue.values.size()==1);std::cout<<\"PASS FIFO ownership: source changed/destroyed after Push, empty gather\\n\";}\n")
  flags=[os.environ.get('CXX','g++'),'-std=c++20','-O1','-g','-pthread','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(p),'-I'+str(SRC)]
  for name in ('parser','sync','allocated','ownership'):
   extra=[SRC/'soc/host1x/syncpoint.cpp'] if name=='sync' else []
   subprocess.run(flags+[str(p/(name+'.cpp'))]+[str(f) for f in extra]+['-o',str(p/name)],check=True)
   subprocess.run([str(p/name)],check=True,timeout=30,env=ENV)
if __name__=='__main__':
 run()
 subprocess.run(['python3',str(ROOT/'tools/video_tests/submit_test.py')],check=True)
