// Template: run.py inserts real production bodies. Scheduler/OS/diagnostics are doubles.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using u8=uint8_t; using u32=uint32_t; using i32=int32_t; using i8=int8_t;
using i64=int64_t; using u64=uint64_t; using KHandle=u32; using Result=u32;
using std::span;
namespace result { constexpr Result InvalidState=1, TimedOut=2, InvalidHandle=3, InvalidCurrentMemory=4, OutOfRange=5, Cancelled=6, InvalidPriority=7; }
namespace constant { constexpr KHandle BaseHandleIndex=0xD000; }
namespace fmt { template<class... A> std::string format(const char* s, A&&...){return s;} template<class T> T* ptr(T* p){return p;} }
template<class... A> std::runtime_error exception(const char* s, A&&...){return std::runtime_error(s);}
template<class... A> void trace(A&&...) {} // Evaluate trace arguments even when not recorded.
#define TRACE_EVENT_FMT(...) trace(__VA_ARGS__)
#define LOGD(...) ((void)0)
#define LOGW(...) ((void)0)
struct AsyncLogger { enum class LogLevel{Debug}; static bool CheckLogLevel(LogLevel){return false;} };
namespace diagnostics {
struct Activity { template<class... A> Activity(A&&...){} };
template<class... A> void Event(A&&...){}
bool SessionTraceEnabled(){return false;}
}
namespace boost::container { template<class T,size_t N> using small_vector=std::vector<T>; }
/*@SPIN@*/
void SpinLock::LockSlow(){while(!try_lock())std::this_thread::yield();}
struct CheckedMutex {
    std::recursive_mutex mutex;
    inline static thread_local int depth{};
    void lock(){mutex.lock();++depth;}
    bool try_lock(){if(!mutex.try_lock())return false;++depth;return true;}
    void unlock(){--depth;mutex.unlock();}
};
namespace type { struct KThread; struct KProcess; struct KSyncObject; }
struct Scheduler;
struct DeviceState {
    inline static thread_local std::shared_ptr<type::KThread> thread;
    std::shared_ptr<type::KProcess> process;
    std::shared_ptr<Scheduler> scheduler;
};
struct Scheduler {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<type::KThread*,int> inserts;
    std::atomic<int> removed{},waited{},timed{},priorityUpdates{};
    std::function<bool()> onTimed;
    std::function<void()> onWait;
    std::function<void(const std::shared_ptr<type::KThread>&)> onInsert;
    void RemoveThread(){++removed;}
    void InsertThread(const std::shared_ptr<type::KThread>& t){
        if(onInsert)onInsert(t);
        {std::lock_guard lock(mutex);++inserts[t.get()];}
        changed.notify_all();
    }
    void WaitSchedule(bool=true){
        assert(CheckedMutex::depth==0);++waited;
        if(onWait)onWait();
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock,std::chrono::seconds(2),[&]{return inserts[DeviceState::thread.get()]>0;}));
    }
    bool TimedWaitSchedule(std::chrono::nanoseconds){assert(CheckedMutex::depth==0);++timed;return onTimed?onTimed():false;}
    void UpdatePriority(const std::shared_ptr<type::KThread>&){++priorityUpdates;}
    int count(const std::shared_ptr<type::KThread>& t){std::lock_guard lock(mutex);return inserts[t.get()];}
};
namespace type {
enum class KType { KThread,KProcess,KSharedMemory,KTransferMemory,KSession,KEvent };
/*@HANDLE_EXCEPTION@*/
struct KObject {
    const DeviceState& state;
    KType objectType;
    KObject(const DeviceState& s,KType t):state(s),objectType(t){}
    virtual ~KObject()=default;
};
struct KSyncObject:KObject {
    using KObject::KObject;
    inline static std::mutex syncObjectMutex;
    std::list<std::shared_ptr<KThread>> syncObjectWaiters;
    bool signalled{};
    void Signal(); bool ResetSignal();
};
struct KThread:KSyncObject,std::enable_shared_from_this<KThread> {
    CheckedMutex& waiterMutex;
    std::atomic<i8> basePriority{20},priority{20};
    u32 *waitMutex{};void *waitConditionVariable{};KHandle waitTag{},handle{};size_t id{};
    std::shared_ptr<KThread> waitThread;
    std::list<std::shared_ptr<KThread>> waiters;
    bool waitSignalled{},cancelSync{},isCancellable{};
    KSyncObject* wakeObject{};Result waitResult{};
    KThread(const DeviceState& s,CheckedMutex& m):KSyncObject(s,KType::KThread),waiterMutex(m){}
    static bool IsHigherPriority(i8 p,const std::shared_ptr<KThread>& t){return p<t->priority;}
    void UpdatePriorityInheritance();
};
struct KEvent:KSyncObject { explicit KEvent(const DeviceState& s):KSyncObject(s,KType::KEvent){} };
struct KSharedMemory:KObject{using KObject::KObject;};
struct KTransferMemory:KObject{using KObject::KObject;};
struct KSession:KSyncObject{using KSyncObject::KSyncObject;};
constexpr u32 HandleWaitersBit=1U<<30;
struct KProcess:KSyncObject {
    CheckedMutex syncWaiterMutex;
    using SyncWaiters=std::multimap<void*,std::shared_ptr<KThread>>;
    SyncWaiters syncWaiters;
    std::shared_mutex handleMutex;
    std::vector<std::shared_ptr<KObject>> handles;
    struct {struct {struct { bool Valid(i8 p){return p>=0&&p<=63;} } priority;} threadInfo;} npdm;
    explicit KProcess(const DeviceState& s):KSyncObject(s,KType::KProcess){}
    /*@HANDLES@*/
    void ClearHandleTable();
    Result MutexLock(const std::shared_ptr<KThread>&,u32*,KHandle,KHandle,bool=false);
    void MutexUnlock(u32*);
    Result ConditionVariableWait(u32*,u32*,KHandle,i64);
    void ConditionVariableSignal(u32*,i32);
    enum class ArbitrationType {WaitIfLessThan,DecrementAndWaitIfLessThan,WaitIfEqual};
    enum class SignalType {Signal,SignalAndIncrementIfEqual,SignalAndModifyBasedOnWaitingThreadCountIfEqual};
    Result WaitForAddress(u32*,u32,i64,ArbitrationType);
    Result SignalToAddress(u32*,u32,i32,SignalType);
};
/*@GENERIC_HANDLE@*/
/*@METHODS@*/
}
struct SvcContext {u64 x0{},x1{},x2{},x3{};u32 w0{},w1{},w2{};};
namespace svc {
/*@SVCS@*/
}
namespace kernel { namespace type=::type; namespace result=::result; }
void dispatch(const DeviceState& state,SvcContext& svcContext,const std::function<void(const DeviceState&,SvcContext&)>& function){
    struct {const char* name; std::function<void(const DeviceState&,SvcContext&)> function;} svc{"test",function};
    /*@DISPATCH@*/
}
struct Fixture {
    DeviceState s;
    Fixture(){s.scheduler=std::make_shared<Scheduler>();s.process=std::make_shared<type::KProcess>(s);s.thread=thread(20);}
    ~Fixture(){s.thread.reset();s.process->syncWaiters.clear();s.process->ClearHandleTable();}
    std::shared_ptr<type::KThread> thread(i8 priority){
        auto t=std::make_shared<type::KThread>(s,s.process->syncWaiterMutex);
        t->priority=priority;t->basePriority=priority;t->handle=s.process->InsertItem(t);t->id=t->handle;return t;
    }
    void queue(const std::shared_ptr<type::KThread>& t,u32* key,u32* mutex){
        std::lock_guard lock(s.process->syncWaiterMutex);
        t->waitConditionVariable=key;t->waitMutex=mutex;t->waitTag=t->handle;t->waitSignalled=false;
        s.process->syncWaiters.emplace(key,t);*key=1;
    }
    template<class F> void as(const std::shared_ptr<type::KThread>& t,F&& f){auto saved=s.thread;s.thread=t;f();s.thread=saved;}
};
void address_tests(){
    Fixture f;u32 v=123;
    assert(f.s.process->WaitForAddress(&v,123,1,type::KProcess::ArbitrationType::WaitIfEqual)==result::TimedOut);
    assert(v==123&&f.s.process->syncWaiters.empty());
    assert(f.s.process->WaitForAddress(&v,123,0,type::KProcess::ArbitrationType::WaitIfEqual)==result::TimedOut);
    assert(f.s.process->WaitForAddress(&v,122,1,type::KProcess::ArbitrationType::WaitIfEqual)==result::InvalidState);
    auto a=f.thread(5),b=f.thread(10);
    f.s.process->syncWaiters.emplace(&v,a);f.s.process->syncWaiters.emplace(&v,b);
    assert(f.s.process->SignalToAddress(&v,0,std::numeric_limits<i32>::min(),type::KProcess::SignalType::Signal)==0);
    assert(v==123&&f.s.process->syncWaiters.empty()&&f.s.scheduler->count(a)==1&&f.s.scheduler->count(b)==1);
    std::cout<<"PASS address timeout preserves game data; INT_MIN wake-all\n";
}
void condition_tests(){
    {Fixture f;u32 key=0,mutex=f.s.thread->handle;
     assert(f.s.process->ConditionVariableWait(&key,&mutex,f.s.thread->handle,0)==result::TimedOut);
     assert(mutex==0&&key==0&&f.s.process->syncWaiters.empty());assert(f.s.scheduler->removed==0&&f.s.scheduler->waited==0);}
    {Fixture f;u32 key=0,mutex=f.s.thread->handle;
     assert(f.s.process->ConditionVariableWait(&key,&mutex,f.s.thread->handle,1)==result::TimedOut);
     assert(mutex==0&&key==0&&f.s.process->syncWaiters.empty());assert(f.s.scheduler->count(f.s.thread)==1);}
    {Fixture f;auto t=f.thread(5);u32 key=0,mutex=123;f.queue(t,&key,&mutex);
     f.s.scheduler->onInsert=[&](const auto& w){assert(w==t&&w->waitResult==result::InvalidState&&w->waitSignalled&&w->waitConditionVariable==nullptr);};
     f.s.process->ConditionVariableSignal(&key,1);assert(t->waitResult==result::InvalidState&&key==0);}
    for(bool timedResult:{false,true}){
     Fixture f;auto waiter=f.s.thread,signaler=f.thread(20);u32 key=0,mutex=waiter->handle;
     f.s.scheduler->onTimed=[&]{f.as(signaler,[&]{f.s.process->ConditionVariableSignal(&key,1);});return timedResult;};
     assert(f.s.process->ConditionVariableWait(&key,&mutex,waiter->handle,1)==0);
     assert(mutex==waiter->handle&&key==0&&f.s.scheduler->count(waiter)==1);
    }
    {Fixture f;auto waiter=f.s.thread,signaler=f.thread(20);u32 key=0,mutex=waiter->handle;
     f.s.scheduler->onWait=[&]{f.as(signaler,[&]{f.s.process->ConditionVariableSignal(&key,1);});};
     assert(f.s.process->ConditionVariableWait(&key,&mutex,waiter->handle,-1)==0);}
    {Fixture f;auto waiter=f.s.thread,owner=f.thread(30);waiter->basePriority=5;waiter->priority=5;u32 key=0,mutex=waiter->handle;
     f.s.scheduler->onTimed=[&]{__atomic_store_n(&mutex,owner->handle,__ATOMIC_SEQ_CST);f.as(owner,[&]{f.s.process->ConditionVariableSignal(&key,1);});assert(owner->priority==5);return false;};
     assert(f.s.process->ConditionVariableWait(&key,&mutex,waiter->handle,1)==result::TimedOut);
     assert(owner->waiters.empty()&&!waiter->waitThread&&owner->priority==30&&f.s.scheduler->count(waiter)==1);}
    std::cout<<"PASS condition zero/positive/infinite timeouts, invalid owner publication, signal/timeout ordering, PI removal\n";
}
void mutex_priority_tests(){
    for(int count:{0,1,4}){
        Fixture f;auto owner=f.s.thread;u32 mutex=owner->handle|type::HandleWaitersBit,other=owner->handle|type::HandleWaitersBit;
        std::vector<std::shared_ptr<type::KThread>> waiters;
        for(int i=0;i<count;++i){auto t=f.thread(static_cast<i8>(5+i));waiters.push_back(t);assert(f.s.process->MutexLock(t,&mutex,owner->handle,t->handle)==0);}
        auto unrelated=f.thread(15);f.s.process->MutexLock(unrelated,&other,owner->handle,unrelated->handle);
        f.s.process->MutexUnlock(&mutex);
        assert(owner->waiters.size()==1&&owner->priority==15&&unrelated->waitThread==owner);
        if(count){assert((mutex&~type::HandleWaitersBit)==waiters[0]->handle&&!waiters[0]->waitThread);assert(waiters[0]->waiters.size()==static_cast<size_t>(count-1));assert(f.s.scheduler->count(waiters[0])==1);for(int i=1;i<count;++i)assert(waiters[i]->waitThread==waiters[0]);}
        else assert(mutex==0);
        for(auto& t:waiters){t->waiters.clear();t->waitThread.reset();}owner->waiters.clear();unrelated->waitThread.reset();
    }
    {Fixture f;auto owner=f.thread(30),waiter=f.s.thread;u32 mutex=owner->handle|type::HandleWaitersBit;
     f.s.scheduler->onWait=[&]{f.as(owner,[&]{f.s.process->MutexUnlock(&mutex);});};
     assert(f.s.process->MutexLock(waiter,&mutex,owner->handle,waiter->handle)==0);
     assert(mutex==waiter->handle&&owner->waiters.empty());}
    {Fixture f;auto a=f.s.thread,b=f.thread(20),c=f.thread(30);a->basePriority=5;a->priority=5;
     a->waitThread=b;b->waiters.push_back(a);b->waitThread=c;c->waiters.push_back(b);a->UpdatePriorityInheritance();assert(b->priority==5&&c->priority==5);
     a->basePriority=15;a->UpdatePriorityInheritance();assert(a->priority==15&&b->priority==15&&c->priority==15);
     SvcContext ctx;ctx.w0=b->handle;ctx.w1=15;svc::SetThreadPriority(f.s,ctx);assert(ctx.w0==0&&b->basePriority==15);
     a->waitThread.reset();b->waiters.clear();b->UpdatePriorityInheritance();assert(b->priority==15&&c->priority==15);
     b->waitThread.reset();c->waiters.clear();c->UpdatePriorityInheritance();assert(c->priority==30);}
    {Fixture f;auto a=f.s.thread,b=f.thread(30);a->basePriority=20;a->priority=5;b->priority=5;
     a->waitThread=b;b->waitThread=a;a->waiters.push_back(b);b->waiters.push_back(a);a->UpdatePriorityInheritance();assert(a->priority==20&&b->priority==20);
     a->waitThread.reset();b->waitThread.reset();a->waiters.clear();b->waiters.clear();}
    std::cout<<"PASS mutex zero/one/multiple/unrelated waiters; synchronous wait unlocks graph; transitive priority and cycle\n";
}
void svc_tests(){
    Fixture f;auto e=std::make_shared<type::KEvent>(f.s);auto h=f.s.process->InsertItem(e);SvcContext ctx;ctx.w0=h;
    svc::ResetSignal(f.s,ctx);assert(ctx.w0==result::InvalidState);e->Signal();ctx.w0=h;svc::ResetSignal(f.s,ctx);assert(ctx.w0==0&&!e->signalled);
    ctx.w0=f.s.thread->handle;svc::ClearEvent(f.s,ctx);assert(ctx.w0==result::InvalidHandle);
    auto t=f.thread(10);f.s.process->CloseHandle(t->handle);ctx.w1=t->handle;svc::GetThreadId(f.s,ctx);assert(ctx.w0==result::InvalidHandle);
    KHandle invalid=0;ctx.x1=reinterpret_cast<u64>(&invalid);ctx.w2=1;svc::WaitSynchronization(f.s,ctx);assert(ctx.w0==result::InvalidHandle);
    ctx.x1=0;ctx.w2=0;ctx.x3=0;svc::WaitSynchronization(f.s,ctx);assert(ctx.w0==result::TimedOut);
    f.s.thread->cancelSync=true;svc::WaitSynchronization(f.s,ctx);assert(ctx.w0==result::Cancelled);
    dispatch(f.s,ctx,[](const auto& s,auto&){s.process->template GetHandle<type::KThread>(0);});assert(ctx.w0==result::InvalidHandle);
    bool propagated=false;try{dispatch(f.s,ctx,[](const auto&,auto&){throw std::out_of_range("unrelated");});}catch(const std::out_of_range&){propagated=true;}assert(propagated);
    std::cout<<"PASS SVC error codes, wrong/closed/invalid handles, empty waits, precise NCE exception boundary\n";
}
void concurrent_tests(){
    for(int i=0;i<250;++i){
        Fixture f;auto owner=f.s.thread,waiter=f.thread(5),signaler=f.thread(30);u32 key=0,mutex=owner->handle;f.queue(waiter,&key,&mutex);std::barrier start(2);
        std::thread a([&]{f.s.thread=owner;start.arrive_and_wait();f.s.process->MutexUnlock(&mutex);f.s.thread.reset();});
        std::thread b([&]{f.s.thread=signaler;start.arrive_and_wait();f.s.process->ConditionVariableSignal(&key,1);f.s.thread.reset();});
        a.join();b.join();assert(mutex==waiter->handle&&key==0&&owner->waiters.empty()&&!waiter->waitThread&&f.s.scheduler->count(waiter)==1);
    }
    for(int i=0;i<200;++i){
        Fixture f;auto waiter=f.s.thread,signaler=f.thread(30);u32 key=0,mutex=waiter->handle;std::barrier ready(2);Result outcome{};
        f.s.scheduler->onTimed=[&]{ready.arrive_and_wait();return false;};
        std::thread a([&]{f.s.thread=waiter;outcome=f.s.process->ConditionVariableWait(&key,&mutex,waiter->handle,1);f.s.thread.reset();});
        std::thread b([&]{f.s.thread=signaler;ready.arrive_and_wait();f.s.process->ConditionVariableSignal(&key,1);f.s.thread.reset();});
        a.join();b.join();assert((outcome==0||outcome==result::TimedOut)&&key==0&&f.s.process->syncWaiters.empty());assert(f.s.scheduler->count(waiter)==1);assert(mutex==(outcome==0?waiter->handle:0));
    }
    std::cout<<"PASS 250 concurrent signal/unlock and 200 concurrent signal/timeout races\n";
}
void handles_spin_tests(){
    Fixture f;std::atomic<KHandle> current{};std::atomic<bool> done{};
    std::thread reader([&]{while(!done){try{auto h=current.load();if(h)assert(f.s.process->GetHandle(h)->objectType==type::KType::KEvent);}catch(const std::out_of_range&){} }});
    for(int i=0;i<4000;++i){auto e=std::make_shared<type::KEvent>(f.s);auto h=f.s.process->InsertItem(e);current=h;f.s.process->CloseHandle(h);}
    done=true;reader.join();bool rejected=false;try{f.s.process->CloseHandle(current);}catch(const std::out_of_range&){rejected=true;}assert(rejected);
    struct Probe: type::KEvent {std::shared_mutex& table;bool& destroyed;Probe(const DeviceState&s,std::shared_mutex&m,bool&d):KEvent(s),table(m),destroyed(d){}~Probe(){assert(table.try_lock());table.unlock();destroyed=true;}};
    for(bool clear:{false,true}){bool destroyed=false;auto p=std::make_shared<Probe>(f.s,f.s.process->handleMutex,destroyed);auto h=f.s.process->InsertItem(p);p.reset();if(clear)f.s.process->ClearHandleTable();else f.s.process->CloseHandle(h);assert(destroyed);}
    RecursiveSpinLock spin;int value=0;std::vector<std::thread> threads;
    for(int i=0;i<8;++i)threads.emplace_back([&]{for(int n=0;n<10000;++n){std::lock_guard lock(spin);std::lock_guard nested(spin);++value;}});
    for(auto& t:threads)t.join();assert(value==80000);
    spin.lock();assert(spin.try_lock());spin.unlock();bool otherAcquired=true;std::thread contender([&]{otherAcquired=spin.try_lock();if(otherAcquired)spin.unlock();});contender.join();assert(!otherAcquired);spin.unlock();
    std::cout<<"PASS 4000 concurrent handle operations, destruction outside lock, 80000 recursive increments and try_lock\n";
}
int main(){address_tests();condition_tests();mutex_priority_tests();svc_tests();concurrent_tests();handles_spin_tests();std::cout<<"ALL HOST TESTS PASSED (fake scheduler; not an Android validation)\n";}
