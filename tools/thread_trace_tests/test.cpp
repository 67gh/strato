#include "common/diagnostic_recorder.h"
#include "common/diagnostic_session.h"
#include "common/thread_activity.h"
#include <cassert>
#include <fstream>
#include <thread>
#include <vector>
#include <filesystem>
using namespace skyline::diagnostics;
using namespace std::chrono_literals;
std::string read(const std::string &path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
 assert(argc==3);std::string dir=argv[1],mode=argv[2];std::filesystem::create_directories(dir);
 ThreadActivities registry;
 registry.Guest(100,0,0xD000,1,false);
 registry.Begin(100,1,10,"svc",26,0xD027,0x1234,"ArbitrateLock");
 registry.Arguments(100,1,3,4,5,6);
 registry.Begin(100,2,20,"ipc",7,0,0,"service");
 auto *slot=registry.Find(100);assert(slot->state.depth==2&&slot->state.isGuest&&slot->state.guest==0);
 registry.End(100,2,30,9);assert(slot->state.depth==1&&slot->state.lastResult==9);
 assert(slot->state.calls[0].extra[3]==6);
 registry.End(100,1,40,0);assert(slot->state.depth==0&&slot->state.completed==2);
 for(unsigned i=0;i<9;++i)registry.Begin(100,100+i,i,"depth",0,0,0,"");
 assert(slot->state.depth==8&&slot->lost==1);
 registry.Guest(100,0,0xD000,50,true);assert(slot->state.exited&&slot->state.depth==0);
 registry.Reset();for(unsigned i=1;i<=257;++i)registry.Guest(i,i,i,1,false);
 assert(registry.overflow==1);registry.Reset();
 auto *contended=registry.Find(1);std::atomic<bool> ready{},release{};
 std::thread holder([&]{std::lock_guard lock(contended->mutex);ready=true;while(!release)std::this_thread::yield();});
 while(!ready)std::this_thread::yield();
 registry.Begin(1,1,1,"blocked",0,0,0,"");release=true;holder.join();assert(contended->lost==1);
 Configure(dir);
 {std::ofstream options(dir+"/diagnostic_options.cfg");if(mode=="legacy")options<<"0 0\n";else options<<"0 0 "<<(mode=="off"?0:1)<<'\n';}
 InitializeRecorder();assert(RecorderEnabled());assert(SessionTraceEnabled()==(mode!="off"));
 GuestThread(5,0xD027);
 auto id=NewId();BeginActivity(id,"svc",26,0xD000,0x123456,"ArbitrateLock");ActivityArguments(id,3,4,5,6);
 {Operation op("ipc_call",7,Timing::Ipc,"test_service");op.Finish(12);}
 Event("probe",42,43,44,"quote\" newline\n");
 if(mode=="io") {std::filesystem::remove(dir+"/session_trace.jsonl");std::filesystem::create_directory(dir+"/session_trace.jsonl");}
 if(mode=="normal"||mode=="budget"){
  std::vector<std::thread> threads;
  for(int n=0;n<8;++n)threads.emplace_back([n]{GuestThread(static_cast<Id>(n+10),static_cast<Id>(n+100));for(int i=0;i<6000;++i)Event("parallel",static_cast<Id>(n),static_cast<Id>(i));GuestThread(static_cast<Id>(n+10),static_cast<Id>(n+100),true);});
  for(auto &t:threads)t.join();
  for(int i=0;i<40000;++i)Event("single_producer_burst",static_cast<Id>(i));
 }
 std::this_thread::sleep_for(1400ms);
 auto live=read(dir+"/thread_activity.json");assert(live.find("ArbitrateLock")!=live.npos);assert(live.find("\"arg5\":6")!=live.npos);
 {std::ofstream f(dir+"/live_snapshot_test.json");f<<live;}
 EndActivity(id,0);GuestThread(5,0xD027,true);ShutdownRecorder();
 assert(!RecorderEnabled());
 auto final=read(dir+"/thread_activity.json");assert(final.find("\"exited\":true")!=final.npos);
}
