#include "common/diagnostic_recorder.h"
#include "common/diagnostic_session.h"
#include "common/diagnostic_crash.h"
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>
using namespace skyline::diagnostics;
int main(int argc,char**argv){
 assert(argc==2);Configure(argv[1]);InitializeRecorder();assert(RecorderEnabled());
 {Scope scope(7,11);Event("test",13,17,19,"quote\" newline\n");}
 Mapping(9,100,100,1000,false,false);Mapping(9,140,20,2000,false,false);Mapping(9,120,10,0,false,true);
 Mapping(9,160,20,0,true,false);Mapping(9,UINT64_MAX,2,0,false,false);
 Unsupported("service",3,"stub");Unsupported("service",3,"stub");
 CountDraw();CountDraw(true);Submitted(7);Completed(7);Presented();
 auto request=RequestCapture();assert(request==1);RequestFrameCapture();assert(ConsumeFrameCapture());assert(!ConsumeFrameCapture());
 SetPaused(true);assert(IsPaused());assert(FlushRecorder("initial"));
 std::filesystem::copy_file(std::filesystem::path(argv[1])/"flight_recorder.jsonl",std::filesystem::path(argv[1])/"initial_events.jsonl");
 const unsigned words[]={0x07230203,0x10000,0,1,0};SaveArtifact("spirv",23,words,sizeof(words),"spv");SaveArtifact("spirv",23,words,sizeof(words),"spv");
 for(int i=0;i<9000;i++)Event("overwrite");
 std::vector<std::thread> threads;for(int t=0;t<4;t++)threads.emplace_back([]{for(int i=0;i<12000;i++)Event("stress");});for(auto&t:threads)t.join();
 assert(FlushRecorder("stress"));InitializeCrashCapture();RecordRawCrash(11,5,7,11,13,17,false);RecordRawCrash(11,99,99,99,99,99,true);CloseCrashCapture();
 ShutdownRecorder();assert(!RecorderEnabled());
}
