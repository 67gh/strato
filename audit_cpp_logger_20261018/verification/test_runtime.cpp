#include <logger/logger.h>
#include <common/exception.h>
#include <common/diagnostic_crash.h>
#include <gpu/diagnostics.h>
#include <services/diagnostic_noops.h>
#include <common/diagnostic_recorder.h>
#include <common/diagnostic_session.h>
#include <atomic>
#include <cassert>
#include <fstream>
#include <thread>
#include <vector>
#include <iostream>
using skyline::AsyncLogger;
using Level=AsyncLogger::LogLevel;
std::atomic<unsigned> androidErrors{};
extern "C" int __android_log_write(int priority,const char*,const char*) { if(priority==6)++androidErrors;return 0; }
std::string read(const std::string &p){std::ifstream f(p);return {std::istreambuf_iterator<char>{f},{}};}
std::vector<void *> skyline::exception::GetStackFrames() { return {}; } // Host fixture: no ARM unwinder.
int main(int argc,char **argv) {
    assert(argc==2);std::string dir=argv[1];std::filesystem::create_directories(dir);
    skyline::diagnostics::Configure(dir);skyline::diagnostics::InitializeRecorder();
    for(int i=0;i<30;i++) {AsyncLogger::Initialize(Level::Debug,dir+"/empty.log");AsyncLogger::Finalize(i%2);AsyncLogger::Finalize();}
    AsyncLogger::Initialize(Level::Warning,dir+"/levels.log");
    AsyncLogger::LogAsync(Level::Debug,std::string{"hidden-direct"});
    AsyncLogger::LogAsync(Level::Error,std::string{"visible-error"});
    AsyncLogger::Finalize(true);
    auto levels=read(dir+"/levels.log");assert(levels.find("hidden-direct")==levels.npos);assert(levels.find("visible-error")!=levels.npos);assert(levels.find("tid=")!=levels.npos);
    AsyncLogger::Initialize(Level::Disabled,dir+"/disabled.log");
    LOGW("unimplemented test feature {}",1); // Must enter registry despite text being disabled.
    AsyncLogger::LogAsync(Level::Error,std::string{"must-not-write"});
    AsyncLogger::Finalize();assert(!std::filesystem::exists(dir+"/disabled.log"));
    AsyncLogger::Initialize(Level::Info,dir+"/rate.log");
    for(int i=0;i<100;i++){LOGI("same site changing value {}",i);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    AsyncLogger::Finalize();auto rate=read(dir+"/rate.log");assert(rate.find("rate_limited")!=rate.npos);
    AsyncLogger::Initialize(Level::Debug,dir+"/stress.log");
    std::vector<std::thread> producers;
    for(int t=0;t<8;t++)producers.emplace_back([t]{for(int i=0;i<10000;i++)LOGD("stress thread={} value={}",t,i);});
    for(auto &t:producers)t.join();
    AsyncLogger::Finalize();assert(read(dir+"/stress.log").find("logger_stats")!=std::string::npos);
    // Concurrent stop: late producers must be rejected, never use freed storage.
    AsyncLogger::Initialize(Level::Debug,dir+"/stop.log");
    std::thread late([]{for(int i=0;i<30000;i++)LOGD("late {}",i);});
    AsyncLogger::Finalize(false);late.join();
    // Simulated file open failure: directory used as a file, logcat fallback survives.
    AsyncLogger::Initialize(Level::Info,dir);LOGI("fallback");AsyncLogger::Finalize();assert(androidErrors>0);
    for(auto &entry:skyline::service::diagnostics_detail::SuccessOnlyFunctions)
        assert(skyline::service::diagnostics_detail::IsSuccessOnly(entry.first,entry.second));
    assert(!skyline::service::diagnostics_detail::IsSuccessOnly("unknown","missing"));
    try { throw skyline::exception("Unimplemented host fixture {}",42); } catch (const skyline::exception&) {}
    skyline::diagnostics::InitializeCrashCapture();
    skyline::diagnostics::RecordRawCrash(11,1,2,3,4,5,false);
    skyline::diagnostics::RecordRawCrash(11,6,7,8,9,10,false);
    skyline::diagnostics::CloseCrashCapture();assert(std::filesystem::file_size(dir+"/native_crash.bin")==96);
    std::filesystem::create_directory(dir+"/gpu_fault.log");
    skyline::gpu::diagnostics::Initialize("");skyline::gpu::diagnostics::DumpFaultReport("host injected open failure");
    unsigned bytes[]={1,2,3,4};skyline::diagnostics::SaveArtifact("spirv",99,bytes,sizeof(bytes),"spv");
    {skyline::diagnostics::Operation op("test_op",7,skyline::diagnostics::Timing::Shader);op.Finish();}
    {skyline::diagnostics::Operation op("test_fail",8,skyline::diagnostics::Timing::Pipeline);}
    skyline::diagnostics::Mapping(1,4096,4096,8192,false,false);
    skyline::diagnostics::ShutdownRecorder();
    assert(read(dir+"/flight_recorder.jsonl").find("gpu_report_open_failed")!=std::string::npos);
    assert(read(dir+"/unsupported.json").find("Unimplemented host fixture")!=std::string::npos);
    assert(read(dir+"/unsupported.json").find("unimplemented test feature")!=std::string::npos);
    assert(read(dir+"/performance_summary.json").find("p95_ns_upper")!=std::string::npos);
    assert(read(dir+"/flight_recorder.jsonl").find("failed_or_abandoned")!=std::string::npos);
    bool artifact=false;for(auto &p:std::filesystem::directory_iterator(dir))if(p.path().extension()==".spv")artifact=true;assert(artifact);
    std::cout<<"PASS runtime: lifecycle, direct filtering, disabled stubs, rate limits, overflow, concurrent stop, I/O failure, async artifact drain, timings and failed operation\n";
}
