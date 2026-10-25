from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1];out=r/'results';out.mkdir(exist_ok=True);s=(r/'app/src/main/cpp/skyline/gpu/fence_cycle.h').read_text();a=s.index('        bool Poll(');b=s.index('\n        /**',a)
pre=r'''
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
using VkDevice=int;using VkFence=int;
namespace vk{enum class Result{eSuccess,eNotReady,eErrorDeviceLost,eErrorOutOfHostMemory};std::string to_string(Result){return "status";}}
namespace fmt{template<class...A>std::string format(const char*s,A...){return s;}int ptr(int p){return p;}}
struct exception:std::runtime_error{template<class...A>exception(const char*s,A...):std::runtime_error(s){}};
int faultReports{},captures{};namespace diagnostics{void CaptureDeviceFault(int){++captures;}void DumpFaultReport(std::string){++faultReports;}}
struct Driver{vk::Result result=vk::Result::eNotReady;vk::Result getFenceStatus(int,int){return result;}operator int(){return 1;}};
struct Device{Driver driver;int dispatcher{};Driver&operator*(){return driver;}int*getDispatcher(){return &dispatcher;}};
struct FenceCycle;
struct Chains{std::vector<std::shared_ptr<FenceCycle>>values;template<class F>bool AllOf(F f){for(auto&v:values)if(!f(v))return false;return true;}};
struct FenceCycle{std::atomic_flag signalled{};std::recursive_mutex mutex;std::shared_mutex chainMutex;Chains chainedCycles;bool submitted{};Device device;int fence{},completed{},destroyed{};std::shared_ptr<FenceCycle>semaphoreUnsignalCycle;void ReportCompletion(){++completed;}void DestroyDependencies(){++destroyed;}
'''
main=r'''
};int main(){FenceCycle c;assert(!c.Poll());assert(!c.Poll(false));c.submitted=true;assert(!c.Poll(false));assert(c.destroyed==0);
 for(auto result:{vk::Result::eErrorDeviceLost,vk::Result::eErrorOutOfHostMemory}){c.device.driver.result=result;bool threw=false;try{c.Poll(false,true);}catch(const exception&){threw=true;}assert(threw&&!c.signalled.test()&&c.destroyed==0);}
 assert(faultReports==2&&captures==1);c.device.driver.result=vk::Result::eSuccess;assert(c.Poll(false,true)&&c.signalled.test()&&c.destroyed==1);std::cout<<"Fence: quick/not-submitted/not-ready, device-lost/OOM propagate without releasing dependencies, success OK\n";}
'''
(out/'fence.cpp').write_text(pre+s[a:b]+main)
p=subprocess.run(['g++','-std=c++20','-pthread',str(out/'fence.cpp'),'-o',str(out/'fence')],capture_output=True,text=True);(out/'fence_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'fence')],capture_output=True,text=True);(out/'fence.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0
