"""Compile source extracts, with explicit host/Vulkan shims. No Android build claim."""
from pathlib import Path
import os, subprocess, json

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent/'app/src/main/cpp/skyline'
if not ROOT.is_dir():
    ROOT=HERE.parent/'source/strato/app/src/main/cpp/skyline'
def clean(p):
    return '\n'.join(l for l in p.read_text().splitlines() if not l.lstrip().startswith(('#include','#pragma once')))
def block(s, needle):
    start=s.index(needle); opening=s.index('{',start); level=1; i=opening+1
    while level:
        if s[i]=='{': level+=1
        elif s[i]=='}': level-=1
        i+=1
    return s[start:i]
common='''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
namespace fmt { template<class... A> std::string format(const char* s,A&&...){return s;} template<class T> T ptr(T x){return x;} }
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define TRACE_EVENT(...) ((void)0)
#define TRACE_EVENT_BEGIN(...) ((void)0)
#define TRACE_EVENT_END(...) ((void)0)
namespace skyline {
using SpinLock=std::mutex;
using SharedSpinLock=std::shared_mutex;
template<class T> using span=std::vector<T>;
struct exception:std::runtime_error { template<class... A> exception(const char* s,A&&...):std::runtime_error(s){} };
namespace diagnostics {
using Id=u64; inline std::atomic<u64> completions{}, events{};
inline Id NewId(){ static std::atomic<Id> id{};return ++id;}
inline void Completed(Id){++completions;}
inline void Submitted(Id){}
template<class... A> void Event(A&&...){++events;}
}
}
'''
quad=common+'''#include <span>
namespace vk {using DeviceSize=u64;enum class IndexType {eUint32,eUint16,eUint8EXT}; }
'''+clean(ROOT/'gpu/interconnect/conversion/quads.h')+'\n'+clean(ROOT/'gpu/interconnect/conversion/quads.cpp')+'''
using namespace skyline::gpu::interconnect::conversion::quads;
template<class T> void indexed(vk::IndexType type){
 for(u32 n=0;n<=19;++n){
  std::vector<T> src(n),out(GetIndexCount(n));
  for(u32 i=0;i<n;++i)src[i]=static_cast<T>(i+11);
  GenerateIndexedQuadConversionBuffer(reinterpret_cast<u8*>(out.data()),reinterpret_cast<u8*>(src.data()),n,type);
  const u32 pattern[]{0,1,2,2,3,0};
  for(size_t i=0;i<out.size();++i)assert(out[i]==(i/6)*4+pattern[i%6]+11);
 }
}
int main(){
 for(u32 n=0;n<=19;++n){
  assert(GetIndexCount(n)==u64{n/4}*6);
  std::vector<u32> out(GetIndexCount(n));
  GenerateQuadListConversionBuffer(out.data(),n,8);
  const u32 pattern[]{0,1,2,2,3,0};
  for(size_t i=0;i<out.size();++i)assert(out[i]==(i/6)*4+pattern[i%6]+8);
 }
 indexed<u8>(vk::IndexType::eUint8EXT);indexed<u16>(vk::IndexType::eUint16);indexed<u32>(vk::IndexType::eUint32);
 assert(GetIndexCount(715827884u)==1073741826ULL);
 assert(GetIndexCount(UINT32_MAX)==6442450938ULL);
 assert(GetRequiredBufferSize(UINT32_MAX,4)==25769803752ULL);
 bool overflow=false;try{GetRequiredBufferSize(8,SIZE_MAX);}catch(const std::length_error&){overflow=true;}assert(overflow);
 u32 out[6];GenerateQuadListConversionBuffer(out,4,UINT32_MAX-3);assert(out[4]==UINT32_MAX);
 overflow=false;try{GenerateQuadListConversionBuffer(out,4,UINT32_MAX-2);}catch(const std::out_of_range&){overflow=true;}assert(overflow);
 bool rejected=false;try{GenerateIndexedQuadConversionBuffer(nullptr,nullptr,0,static_cast<vk::IndexType>(9));}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 std::cout<<"PASS quads: counts 0..19, uint8/16/32, first=8, overflow, invalid type\\n";
}
'''
# Compile and exercise the actual cached non-indexed helper, including growth
# inside an existing allocation's page padding and changing the first vertex.
cache_shims='''
namespace skyline::gpu::interconnect::maxwell3d {
constexpr size_t PAGE_SIZE=4096;
namespace util{inline u64 AlignUp(u64 n,u64 a){return (n+a-1)/a*a;}}
namespace memory{struct Buffer{std::vector<u32> storage;int vkBuffer{1};Buffer(size_t n):storage(n/4){};template<class T>std::span<T> cast(){return {reinterpret_cast<T*>(storage.data()),storage.size()*4/sizeof(T)};}};}
struct FakeMemory{memory::Buffer AllocateBuffer(u64 bytes){return memory::Buffer(bytes);}};
struct CacheContext{struct{FakeMemory memory;}gpu;struct{std::vector<std::shared_ptr<void>> deps;void AttachDependency(std::shared_ptr<void> p){deps.push_back(std::move(p));}}executor;};
struct Maxwell3D{CacheContext ctx;std::shared_ptr<memory::Buffer> quadConversionBuffer;bool quadConversionBufferAttached{};u32 quadConversionFirstVertex{},quadConversionVertexCount{};vk::DeviceSize UpdateQuadConversionBuffer(u32,u32);};
'''
cache_source=(ROOT/'gpu/interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
cache_shims+=block(cache_source,'vk::DeviceSize Maxwell3D::UpdateQuadConversionBuffer(')+'\n}\n'
at=quad.index('int main(){')
quad=quad[:at]+cache_shims+quad[at:]
quad=quad.replace('int main(){','''int main(){
 {skyline::gpu::interconnect::maxwell3d::Maxwell3D renderer;
  renderer.UpdateQuadConversionBuffer(4,8);auto old=renderer.quadConversionBuffer;
  renderer.UpdateQuadConversionBuffer(8,8);auto grown=renderer.quadConversionBuffer;
  assert(old!=grown&&grown->storage[10]==15);
  renderer.UpdateQuadConversionBuffer(4,3);assert(renderer.quadConversionBuffer->storage[0]==3);
  assert(old->storage[0]==8); // prior queued allocation stays immutable
 }
''')
(HERE/'test_quads.cpp').write_text(quad)
fake_vk='''
using VkFence=void*;using VkDevice=void*;
namespace vk {
enum class Result {eSuccess,eTimeout,eErrorInitializationFailed,eErrorDeviceLost};
inline std::string to_string(Result){return "fake-vulkan-result";}
struct Fence { void* ptr{};operator VkFence() const{return ptr;} };
struct Semaphore {};
struct RawDevice {
 std::atomic<bool> ready{},error{};std::atomic<unsigned> waits{};
 Result waitForFences(int,const Fence*,bool,u64 timeout,int) const {
  auto& self=const_cast<RawDevice&>(*this);++self.waits;
  assert(timeout==50000000ULL);
  if(error)return Result::eErrorDeviceLost;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return ready?Result::eSuccess:Result::eTimeout;
 }
 Result getFenceStatus(Fence,int)const{return ready?Result::eSuccess:Result::eTimeout;}
 operator VkDevice()const{return nullptr;}
};
namespace raii { struct Device {
 mutable RawDevice raw;int dispatcher{};
 const RawDevice& operator*()const{return raw;}
 void resetFences(Fence)const{raw.ready=false;}
 const int* getDispatcher()const{return &dispatcher;}
}; }
}
namespace skyline::gpu::diagnostics {
inline void CaptureDeviceFault(VkDevice){}
inline void DumpFaultReport(std::string){}
}
'''
gpu=common+fake_vk+clean(ROOT/'common/atomic_forward_list.h')+'\n'+clean(ROOT/'gpu/fence_cycle.h')
header=(ROOT/'gpu/interconnect/command_executor.h').read_text()
impl=(ROOT/'gpu/interconnect/command_executor.cpp').read_text()
gpu+='''
namespace skyline {
struct FakeSettings {bool setting{};u32 scale{};bool* forceMaxGpuClocks{&setting};u32* executorSlotCountScale{&scale};};
struct FakeProcess {std::atomic<unsigned> killed{};void Kill(bool){++killed;}};
struct DeviceState {std::shared_ptr<FakeSettings> settings{std::make_shared<FakeSettings>()};std::shared_ptr<FakeProcess> process{std::make_shared<FakeProcess>()};};
}
inline void adrenotools_set_turbo(bool){}
namespace skyline::gpu::interconnect {
'''+block(header,'class ExecutionWaiterThread')+';\n'
start=impl.index('    void ExecutionWaiterThread::Run()')
end=impl.index('    void CheckpointPollerThread::Run()',start)
gpu+=impl[start:end]+'\n}\n'
# Test the real Stop choreography with the real CircularQueue. The recording
# body and slot Vulkan objects are shims; full ProcessSlot is not exercised.
gpu+=clean(ROOT/'common/circular_queue.h')+'''
namespace skyline::constant {constexpr size_t NsInMillisecond=1000000;}
inline vk::raii::Device recordDevice;
inline std::atomic<bool> recordStarted{};
inline std::atomic<bool> blockRecordOutput{};
namespace skyline::gpu::interconnect {
'''
record=block(header,'class CommandRecordThread')
slot=block(record,'struct Slot')
record=record.replace(slot,'''struct Slot {
 std::mutex beginLock;std::condition_variable beginCondition;bool retired{};
 std::shared_ptr<FenceCycle> cycle{std::make_shared<FenceCycle>(recordDevice,vk::Fence{},vk::Semaphore{},true)};
}''')
gpu+=record+';\n'
a=impl.index('    CommandRecordThread::CommandRecordThread(');b=impl.index('    CommandRecordThread::Slot::ScopedBegin::ScopedBegin',a)
gpu+=impl[a:b]
gpu+=block(impl,'CommandRecordThread::~CommandRecordThread()')+'\n'
gpu+=block(impl,'void CommandRecordThread::Stop()')+'\n'
gpu+='''
void CommandRecordThread::Run(){
 try{
  outgoing.Push(&slots.emplace_back());recordStarted=true;
  if(blockRecordOutput)outgoing.Push(&slots.front());
  incoming.Process([](Slot*){},[]{});
 }catch(...){}
 outgoing.Close();incoming.Close();
}
}
'''
gpu+='''
using namespace skyline;using namespace skyline::gpu;using namespace skyline::gpu::interconnect;
int main(){
 vk::raii::Device device;
 {auto cycle=std::make_shared<FenceCycle>(device,vk::Fence{},vk::Semaphore{});
  auto waiter=std::async(std::launch::async,[&]{cycle->Wait(true);});
  cycle->Cancel();assert(waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready);waiter.get();}
 {auto cycle=std::make_shared<FenceCycle>(device,vk::Fence{},vk::Semaphore{});
  auto dependency=std::make_shared<int>(1);std::weak_ptr<int> weak=dependency;
  cycle->AttachObject(dependency);dependency.reset();cycle->NotifySubmitted();
  auto waiter=std::async(std::launch::async,[&]{cycle->Wait(true);});
  while(device.raw.waits==0)std::this_thread::yield();
  cycle->Cancel();assert(!weak.expired());assert(!cycle->Poll());
  device.raw.ready=true;waiter.get();assert(weak.expired());}
 {auto cycle=std::make_shared<FenceCycle>(device,vk::Fence{},vk::Semaphore{});
  cycle->NotifySubmitted();device.raw.error=true;bool failed=false;
  try{cycle->Wait();}catch(const std::exception&){failed=true;}assert(failed);
  cycle.reset();device.raw.error=false;}
 DeviceState state;
 for(unsigned repeat=0;repeat<100;++repeat){
  std::atomic<unsigned> count{};
  {ExecutionWaiterThread worker(state);for(unsigned i=0;i<100;++i)worker.Queue(nullptr,[&]{++count;});worker.Stop();worker.Stop();}
  assert(count==100);
 }
 {ExecutionWaiterThread worker(state);
  std::promise<void> entered, release;auto gate=release.get_future().share();
  worker.Queue(nullptr,[&]{entered.set_value();gate.wait();throw std::runtime_error("injected callback failure");});
  entered.get_future().wait();
  std::future<void> completion;
  {auto promise=std::make_shared<std::promise<void>>();completion=promise->get_future();worker.Queue(nullptr,[promise]{promise->set_value();});}
  release.set_value();
  assert(completion.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  bool broken=false;try{completion.get();}catch(const std::future_error&){broken=true;}assert(broken);
  worker.Stop();bool rejects=false;try{worker.Queue(nullptr,[]{});}catch(const std::exception&){rejects=true;}assert(rejects);
 }
 for(bool blocked:{false,true}){
  recordStarted=false;blockRecordOutput=blocked;
  CommandRecordThread recorder(state);
  while(!recordStarted)std::this_thread::yield();
  recorder.Stop();recorder.Stop();
 }
 assert(state.process->killed==1);
 std::cout<<"PASS GPU host shims: pre-submit cancellation, submitted resource retention, device loss, worker startup/drain/repeated stop/callback failure/broken promise\\n";
}
'''
(HERE/'test_gpu_workers.cpp').write_text(gpu)
index_shims='''
#include <span>
namespace vk {using DeviceSize=u64;using PipelineStageFlags=u32;enum class PipelineStageFlagBits:u32{eVertexInput=1};enum class IndexType{eUint32,eUint16,eUint8EXT};}
namespace skyline::gpu::interconnect {
namespace engine {struct IndexBuffer{enum class IndexSize{OneByte,TwoBytes,FourBytes};IndexSize indexSize{IndexSize::FourBytes};u64 address{0x1000},limit{0x10ff};};}
struct DirtyManager{template<class... A>void Bind(A&&...){};};
namespace dirty {using Handle=int;struct RefreshableManualDirty{};struct CachedManualDirty{};template<class T>struct BoundSubresource{const T& value;BoundSubresource(DirtyManager&,int,const T& v):value(v){};const T* operator->()const{return &value;}};}
struct BufferBinding {int buffer{};size_t offset{},size{};explicit operator bool()const{return buffer!=0;}bool operator==(const BufferBinding&)const=default;};
struct FakeBuffer {bool dirty{};void PopulateReadBarrier(vk::PipelineStageFlagBits,u32&,u32&){};bool IsCurrentExecutionGpuDirty(){return dirty;}};
struct BufferView {
 FakeBuffer* buffer{};u8* data{};u64 size{};
 explicit operator bool()const{return buffer!=nullptr;}
 FakeBuffer* GetBuffer(){return buffer;}
 std::span<u8> GetReadOnlyBackingSpan(bool,const std::function<void()>& callback){if(buffer->dirty)callback();return {data,static_cast<size_t>(size)};}
 void Read(bool,const std::function<void()>& callback,std::vector<u8>& dest,size_t offset){if(buffer->dirty)callback();std::copy_n(data+offset,dest.size(),dest.begin());}
 template<class... A>BufferBinding TryMegaBuffer(A&&...){return {};}
};
struct FakeMega {std::vector<u8> bytes;struct Allocation{int buffer{1};size_t offset{};std::span<u8> region;};Allocation Allocate(int,u64 size){bytes.resize(size);return {1,0,{bytes}};}};
struct FakeExecutor {int cycle{},executionTag{};void AttachBuffer(BufferView&){};};
struct FakeGpu {FakeMega megaBufferAllocator;};
struct InterconnectContext {FakeExecutor executor;FakeGpu gpu;FakeBuffer buffer;std::vector<u8> mapping;u64 requested{};};
struct CachedMappedBufferView {
 BufferView view;
 void Update(InterconnectContext& ctx,u64,u64 size,bool){ctx.requested=size;view={size&&ctx.mapping.size()?&ctx.buffer:nullptr,ctx.mapping.data(),std::min<u64>(size,ctx.mapping.size())};}
 void PurgeCaches(){view={};}BufferView& operator*(){return view;}BufferView* operator->(){return &view;}
};
struct StateUpdateBuilder {unsigned binds{};void SetIndexBuffer(const BufferBinding&,vk::IndexType){++binds;}void SetIndexBuffer(const BufferView&,vk::IndexType){++binds;}};
struct BufferReadNeedsSubmission {};
struct ContextLock{template<class... A>ContextLock(A&&...){};bool IsFirstUsage(){return false;}};
struct CommandExecutor{int tag{};};
struct ConstantBuffer{BufferView view;void Read(CommandExecutor&,span<u8>,size_t);};
}
'''
active=(ROOT/'gpu/interconnect/maxwell_3d/active_state.cpp').read_text()
active_header=(ROOT/'gpu/interconnect/maxwell_3d/active_state.h').read_text()
index=common+index_shims+clean(ROOT/'gpu/interconnect/conversion/quads.h')+'\n'+clean(ROOT/'gpu/interconnect/conversion/quads.cpp')
index+='\nnamespace skyline::gpu::interconnect::maxwell3d {\n'+block(active_header,'class IndexBufferState')+';\n'
a=active.index('    static vk::DeviceSize GetIndexBufferSize(');b=active.index('    /* Transform Feedback Buffer',a)
index+=active[a:b]+'\n}\n'
# The real ConstantBuffer read callback is exercised as well as indexed readback.
constant=(ROOT/'gpu/interconnect/common/common.cpp').read_text()
index+='\nnamespace skyline::gpu::interconnect {\n'+block(constant,'void ConstantBuffer::Read(')+'\n}\n'
index+='''
using namespace skyline::gpu::interconnect;using namespace skyline::gpu::interconnect::maxwell3d;
int main(){
 engine::IndexBuffer registers;IndexBufferState::EngineRegisters binding{registers};DirtyManager manager;
 IndexBufferState state(0,manager,binding);InterconnectContext ctx;StateUpdateBuilder builder;u32 src{},dst{};
 ctx.mapping.resize(24);state.Flush(ctx,builder,src,dst,false,false,2,4);assert(state.IsValid()&&builder.binds==1&&ctx.requested==24);
 ctx.mapping.resize(20);state.Flush(ctx,builder,src,dst,false,false,2,4);assert(!state.IsValid()&&builder.binds==1);
 assert(state.Refresh(ctx,builder,src,dst,false,false,2,4));
 ctx.mapping.clear();state.Flush(ctx,builder,src,dst,false,false,2,4);assert(!state.IsValid());
 ctx.mapping.resize(24);state.Flush(ctx,builder,src,dst,true,false,2,4);assert(state.IsValid()&&builder.binds==2);
 state.PurgeCaches();assert(!state.IsValid());
 state.Flush(ctx,builder,src,dst,false,false,UINT32_MAX,4);assert(!state.IsValid()&&ctx.requested==17179869196ULL);
 ctx.mapping.resize(256);state.Flush(ctx,builder,src,dst,false,true,0,0);assert(state.IsValid()&&ctx.requested==256);
 registers.limit=0xfff;state.Flush(ctx,builder,src,dst,false,true,0,0);assert(!state.IsValid()&&ctx.requested==0);
 assert(GetIndexBufferSize(engine::IndexBuffer::IndexSize::FourBytes,1073741824ULL)==4294967296ULL);
 BufferView small{&ctx.buffer,ctx.mapping.data(),4};bool rejected=false;
 try{GenerateQuadConversionIndexBuffer(ctx,engine::IndexBuffer::IndexSize::FourBytes,small,1,4);}catch(const std::exception&){rejected=true;}assert(rejected);
 ctx.buffer.dirty=true;BufferView enough{&ctx.buffer,ctx.mapping.data(),16};bool retry=false;
 try{GenerateQuadConversionIndexBuffer(ctx,engine::IndexBuffer::IndexSize::FourBytes,enough,0,4);}catch(const BufferReadNeedsSubmission&){retry=true;}assert(retry);
 ConstantBuffer cbuf{enough};CommandExecutor executor;std::vector<u8> dest(4);retry=false;
 try{cbuf.Read(executor,dest,0);}catch(const BufferReadNeedsSubmission&){retry=true;}assert(retry);
 ctx.buffer.dirty=false;cbuf.Read(executor,dest,0);
 std::cout<<"PASS index state host shims: exact/partial/missing mapping, retry, purge, large arithmetic, inclusive range, bounds, dirty readback request\\n";
}
'''
(HERE/'test_index_state.cpp').write_text(index)
results=[]
for stem in ['test_quads','test_gpu_workers','test_index_state']:
 command=['g++','-std=c++20','-O1','-g','-pthread','-Wall','-Wextra','-Wno-unknown-pragmas','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(HERE/(stem+'.cpp')),'-o',str(HERE/stem)]
 build=subprocess.run(command,text=True,capture_output=True)
 (HERE/(stem+'_build.log')).write_text(' '.join(command)+'\n'+build.stdout+build.stderr)
 if build.returncode:print(build.stderr);raise SystemExit(build.returncode)
 run=subprocess.run(['timeout','20s',str(HERE/stem)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
 (HERE/(stem+'_run.log')).write_text(run.stdout+run.stderr)
 print(run.stdout+run.stderr)
 results.append({'test':stem,'exit':run.returncode})
 if run.returncode:raise SystemExit(run.returncode)
(HERE/'TEST_RESULTS.json').write_text(json.dumps(results,indent=2))
