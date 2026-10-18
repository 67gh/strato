#!/usr/bin/env python3
"""Compile production watchdog formatting with inaccessible loader/wait metadata."""
from pathlib import Path
import tempfile,subprocess,os
project=Path(__file__).resolve().parents[2]
text=(project/'app/src/main/cpp/skyline/nce/hang_watchdog.h').read_text()
def extract(marker):
    a=text.index(marker);b=text.index('{',a);e=b+1;d=1
    # DescribeThread's default {} occurs before the function body.
    if marker.startswith('static std::string DescribeThread'):
        b=text.index(') {',a)+2;e=b+1;d=1
    while d:d+=(text[e]=='{')-(text[e]=='}');e+=1
    return text[a:e]
source=r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <fmt/format.h>
struct DeviceState {}; // Deliberately no loader: formatting must not access it.
namespace kernel::type {struct KThread {unsigned id=7,handle=42;std::atomic<int>priority{3},basePriority{4};};}
"""+extract('static std::string Symbolize(')+'\n'+extract('static std::string DescribeThread(')+r"""
int main(){
 DeviceState state;
 auto s=Symbolize(state,{reinterpret_cast<void*>(1),reinterpret_cast<void*>(UINTPTR_MAX)});
 assert(s.find("0x1")!=s.npos && s.find("0xFFFFFFFFFFFFFFFF")!=s.npos);
 kernel::type::KThread t;assert(DescribeThread(&t,"sleeping").find("sleeping")!=s.npos);
 assert(DescribeThread(nullptr)=="(unknown)");
}
"""
import argparse
p=argparse.ArgumentParser();p.add_argument('--fmt',type=Path,default=project/'app/libraries/fmt');args=p.parse_args()
with tempfile.TemporaryDirectory() as d:
    root=Path(d);(root/'test.cpp').write_text(source)
    # Parse the whole production class, including code between extracted methods.
    shim=r"""
#include <memory>
#include <map>
#include <array>
#include <fmt/format.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <initializer_list>
using u64=uint64_t;using u32=uint32_t;
struct ucontext {struct {u64 pc{},sp{},regs[31]{};}uc_mcontext;};
struct siginfo {};
namespace skyline {
struct AsyncLogger {static void UpdateTag(){}};
namespace kernel::type {struct KThread {unsigned id{},handle{};std::atomic<int>priority{},basePriority{};};}
struct Process {std::mutex threadMutex,syncWaiterMutex;std::vector<std::shared_ptr<kernel::type::KThread>>threads;std::multimap<void*,std::shared_ptr<kernel::type::KThread>>syncWaiters;};
struct Scheduler {struct Core {std::mutex mutex;int preemptionPriority{};std::vector<std::shared_ptr<kernel::type::KThread>>queue;};std::array<Core,4>cores;std::mutex parkedMutex;std::vector<std::shared_ptr<kernel::type::KThread>>parkedQueue;};
struct DeviceState {Process*process{};Scheduler*scheduler{};};
namespace diagnostics {struct Progress {u64 draws{},dispatches{},submitted{},completed{},frames{};};inline Progress GetProgress(){return {};}
inline bool IsPaused(){return false;}inline u64 CaptureRequest(){return 0;}inline std::string JsonEscape(const std::string&s){return s;}
inline bool WriteJson(const char*,const std::string&){return true;}inline bool FlushRecorder(const char*){return true;}
inline void Event(const char*){} inline void Feature(const char*,const char*){}}
namespace signal {template<class T>void SetGuestSignalHandler(std::initializer_list<int>,T){} template<class T>void SetHostSignalHandler(std::initializer_list<int>,T){}}
}
#define LOGINF(...) ((void)fmt::format(__VA_ARGS__))
#define LOGW(...) ((void)fmt::format(__VA_ARGS__))
"""
    filtered='\n'.join(line for line in text.splitlines() if not (line.startswith('#include <common') or line.startswith('#include <loader') or line.startswith('#include <kernel') or line=='#pragma once'))
    (root/'whole.cpp').write_text(shim+filtered)
    subprocess.run(['g++','-std=c++20','-DFMT_HEADER_ONLY','-I'+str(args.fmt/'include'),'-fsyntax-only',str(root/'whole.cpp')],check=True)
    subprocess.run(['g++','-std=c++20','-DFMT_HEADER_ONLY','-I'+str(args.fmt/'include'),'-fsanitize=address,undefined','-no-pie',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
print('PASS: watchdog raw formatting never touches guest symbols or mutable wait owners (ASan/UBSan)')
