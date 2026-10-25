import os
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1];out=r/'results';out.mkdir(exist_ok=True);s=(r/'app/src/main/cpp/skyline/gpu/graphics_pipeline_assembler.cpp').read_text()
def block(text,key):
 a=text.index(key);b=text.index('{',a);i=b+1;d=1
 while d:d+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[a:i]
release=block(s,'void GraphicsPipelineAssembler::ReleasePipelineDescription(')
async_=block(s,'GraphicsPipelineAssembler::CompiledPipeline GraphicsPipelineAssembler::AssemblePipelineAsync(')
assemble=block(s,'vk::raii::Pipeline GraphicsPipelineAssembler::AssemblePipeline(')
a=assemble.index('        boost::container::');b=assemble.index('        descriptionGuard.reset();');assemble=assemble[:a]+'''        if (failure == 4) throw std::runtime_error("renderpass/pipeline failed");
        vk::raii::Pipeline pipeline;
'''+assemble[b:]
pre=r'''
#include <memory>
#include <functional>
#include <future>
#include <vector>
#include <list>
#include <mutex>
#include <span>
#include <cassert>
#include <iostream>
using u32=unsigned;template<class T>using span=std::span<T>;
int failure{},destroyed{};
struct RawDevice{void destroyShaderModule(int,std::nullptr_t,int){++destroyed;}};
struct Device{RawDevice raw;int dispatch{};RawDevice&operator*(){return raw;}int*getDispatcher(){return &dispatch;}};
namespace vk{
struct DescriptorSetLayoutBinding{};struct PushConstantRange{};using PipelineLayout=int;
enum class DescriptorSetLayoutCreateFlagBits{ePushDescriptorKHR};struct DescriptorSetLayoutCreateFlags{DescriptorSetLayoutCreateFlags(){}DescriptorSetLayoutCreateFlags(DescriptorSetLayoutCreateFlagBits){}};
struct DescriptorSetLayoutCreateInfo{DescriptorSetLayoutCreateFlags flags;const DescriptorSetLayoutBinding*pBindings;u32 bindingCount;};
struct PipelineLayoutCreateInfo{const int*pSetLayouts;u32 setLayoutCount;const PushConstantRange*pPushConstantRanges;u32 pushConstantRangeCount;};
namespace raii {struct Pipeline{};struct DescriptorSetLayout{int h{};DescriptorSetLayout(Device&,DescriptorSetLayoutCreateInfo){if(failure==1)throw std::runtime_error("layout failed");}const int&operator*()const{return h;}};struct PipelineLayout{int h{};PipelineLayout(Device&,PipelineLayoutCreateInfo){}const int&operator*()const{return h;}};}
}
struct GPU{Device vkDevice;struct{bool supportsPushDescriptors{};}traits;};
struct Pool{template<class F,class O,class I>std::shared_future<vk::raii::Pipeline> submit(F f,O o,I it,int layout){if(failure==3)throw std::runtime_error("enqueue failed");std::promise<vk::raii::Pipeline>p;try{p.set_value((o->*f)(it,layout));}catch(...){p.set_exception(std::current_exception());}return p.get_future().share();}};
struct GraphicsPipelineAssembler{
 struct Stage{int module;};struct PipelineState{std::vector<Stage>shaderStages;bool destroyShaderModules;};
 struct PipelineDescription{std::vector<Stage>shaderStages;bool destroyShaderModules;PipelineDescription(const PipelineState&s):shaderStages(s.shaderStages),destroyShaderModules(s.destroyShaderModules){if(failure==2)throw std::bad_alloc();}};
 struct CompiledPipeline{vk::raii::DescriptorSetLayout set;vk::raii::PipelineLayout layout;std::shared_future<vk::raii::Pipeline> pipeline;CompiledPipeline(vk::raii::DescriptorSetLayout s,vk::raii::PipelineLayout l,std::shared_future<vk::raii::Pipeline>p):set(std::move(s)),layout(std::move(l)),pipeline(std::move(p)) {}};
 GPU gpu;std::mutex mutex;Pool pool;std::function<void()>compilationCallback;std::list<PipelineDescription>compilePendingDescs;
 void ReleasePipelineDescription(std::list<PipelineDescription>::iterator);
 vk::raii::Pipeline AssemblePipeline(std::list<PipelineDescription>::iterator,vk::PipelineLayout);
 CompiledPipeline AssemblePipelineAsync(const PipelineState&,span<const vk::DescriptorSetLayoutBinding>,span<const vk::PushConstantRange>,bool);
};
'''
main=r'''
int main(){for(bool own:{false,true})for(failure=0;failure<=5;failure++){GraphicsPipelineAssembler a;destroyed=0;GraphicsPipelineAssembler::PipelineState s{{{1},{2}},own};a.compilationCallback=[&]{std::scoped_lock lock(a.mutex);if(failure==5)throw std::runtime_error("callback");};bool threw=false;try{auto p=a.AssemblePipelineAsync(s,{},{},true);p.pipeline.get();}catch(...){threw=true;}assert(threw==(failure!=0));assert(a.compilePendingDescs.empty());assert(destroyed==(own?2:0));}std::cout<<"Pipeline ownership guards: success, layout/description/enqueue/worker/callback failure; owned modules released exactly once, borrowed modules retained, descriptions erased OK\n";}
'''
(out/'lifetime.cpp').write_text(pre+release+assemble+async_+main)
p=subprocess.run(['g++','-std=c++20','-pthread','-fsanitize=address,undefined','-no-pie',str(out/'lifetime.cpp'),'-o',str(out/'lifetime')],capture_output=True,text=True,env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"));(out/'lifetime_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'lifetime')],capture_output=True,text=True,env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"));(out/'lifetime.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0

# Repeat against the exact BS::thread_pool revision used by the repository.
real_pre = pre.replace('#include <memory>', '#include "../tests/vendor/BS_thread_pool.hpp"\n#include <memory>')
a=real_pre.index('struct Pool{');b=real_pre.index('struct GraphicsPipelineAssembler',a)
real_pre=real_pre[:a]+"struct Pool { BS::thread_pool pool{2}; template<class F,class O,class I>std::future<vk::raii::Pipeline> submit(F f,O o,I it,int layout) { if(failure==3)throw std::runtime_error(\"enqueue failed\");return pool.submit(f,o,it,layout); } void wait_for_tasks(){pool.wait_for_tasks();} };\n"+real_pre[b:]
real_pre=real_pre.replace('GPU gpu;std::mutex mutex;', '~GraphicsPipelineAssembler(){pool.wait_for_tasks();} GPU gpu;std::mutex mutex;')
(out/'lifetime_threaded.cpp').write_text(real_pre+release+assemble+async_+main)
p=subprocess.run(['g++','-std=c++20','-pthread','-fsanitize=address,undefined','-no-pie',str(out/'lifetime_threaded.cpp'),'-o',str(out/'lifetime_threaded')],capture_output=True,text=True);(out/'lifetime_threaded_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'lifetime_threaded')],capture_output=True,text=True,env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'));(out/'lifetime_threaded.log').write_text(p.stdout+p.stderr);print('Actual pinned BS::thread_pool: '+p.stdout+p.stderr);assert p.returncode==0
