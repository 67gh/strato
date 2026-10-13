// SPDX-License-Identifier: MPL-2.0
// Host harness. The runner injects the production methods and SpinLock class.
// Guest signal delivery, ARM LockSlow assembly and preemption timers are mocked.
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <kernel/scheduler_timing.h>

#define LOGD(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#define TRACE_EVENT(...) ((void)0)
using namespace std::chrono_literals;

namespace skyline {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
using i8 = std::int8_t;
// @PRODUCTION_SPIN_LOCK@
void SpinLock::LockSlow() { while (!try_lock()) std::this_thread::yield(); }
template<class... T> std::runtime_error exception(const char *message, T&&...) {
    return std::runtime_error(message);
}
namespace util {
    inline std::atomic<u64> ticks{21000};
    u64 GetTimeTicks() { return ticks.load(); }
}
namespace kernel {
namespace type {
struct KThread {
    size_t id{};
    std::atomic<i8> priority{46};
    u8 coreId{}, idealCore{};
    std::bitset<4> affinityMask{1};
    std::recursive_mutex coreMigrationMutex;
    @SCHEDULE_CONDITION_TYPE@ scheduleCondition;
    bool isPaused{}, insertThreadOnResume{}, pendingYield{}, forceYield{}, isPreempted{};
    u64 timesliceStart{}, averageTimeslice{};
    void SendSignal(int) {} // Guest/native signal delivery is outside this host test.
    void ArmPreemptionTimer(std::chrono::nanoseconds) { isPreempted = true; }
    void DisarmPreemptionTimer() { isPreempted = false; }
    static bool IsHigherPriority(i8 priority, const std::shared_ptr<KThread> &thread) {
        return priority < thread->priority;
    }
};
}
struct DeviceState {
    inline static thread_local std::shared_ptr<type::KThread> thread;
};
struct Scheduler {
    const DeviceState &state;
    struct CoreContext {
        u8 id;
        i8 preemptionPriority;
        SpinLock mutex;
        std::list<std::shared_ptr<type::KThread>> queue;
        CoreContext(u8 id, i8 priority) : id(id), preemptionPriority(priority) {}
    };
    std::array<CoreContext,4> cores{CoreContext(0,59),CoreContext(1,59),CoreContext(2,59),CoreContext(3,63)};
    static constexpr std::chrono::milliseconds PreemptiveTimeslice{10};
    static constexpr int YieldSignal{1};
    inline static thread_local bool YieldPending{};
    explicit Scheduler(const DeviceState &state) : state(state) {}
    CoreContext &GetOptimalCoreForThread(const std::shared_ptr<type::KThread> &thread);
    void YieldThread(const std::shared_ptr<type::KThread> &thread);
    void InsertThread(const std::shared_ptr<type::KThread> &thread);
    void MigrateToCore(const std::shared_ptr<type::KThread> &thread, CoreContext *&currentCore,
                      CoreContext *targetCore, std::unique_lock<SpinLock> &lock);
    void WaitSchedule(bool loadBalance = true);
    bool TimedWaitSchedule(std::chrono::nanoseconds timeout);
    void Rotate(bool cooperative = true);
    void RemoveThread();
};
// @PRODUCTION_METHODS@
}
}

using namespace skyline;
using namespace skyline::kernel;

static void Timing() {
    using namespace scheduler_detail;
    assert(UpdateAverage(80,20900,21000)==95);
    assert(UpdateAverage(80,20900000000ULL,20900000100ULL)==95);
    assert(UpdateAverage(80,0,100)==80);
    assert(UpdateAverage(80,101,100)==80);
    assert(RemainingTimeslice(100,1000,1030)==70);
    assert(RemainingTimeslice(100,1000,1100)==1);
    assert(RemainingTimeslice(100,1000,1200)==1);
    assert(RemainingTimeslice(100,0,1200)==100);
    assert(RemainingTimeslice(0,0,0)==1);
    assert(RemainingTimeslice(0,100,150)==50);
    constexpr u64 maximum = std::numeric_limits<u64>::max();
    assert(UpdateAverage(maximum,1,maximum)==maximum-1);
    assert(AddTimeslices(maximum-5,10)==maximum);
    std::mt19937_64 rng(20261012);
    for (unsigned i=0; i<100000; ++i) {
        u64 previous=rng(), elapsed=rng()>>1;
        auto expected=static_cast<u64>((static_cast<unsigned __int128>(previous)+3*static_cast<unsigned __int128>(elapsed))/4);
        assert(UpdateAverage(previous,1,elapsed+1)==expected);
    }
    auto delay=20ms;
    for(auto expected : {40ms,80ms,160ms,160ms,160ms}) {
        delay=NextLoadBalanceDelay(delay);
        assert(delay==expected);
    }
    assert(NextLoadBalanceDelay(std::chrono::milliseconds::max())==160ms);
    std::cout << "PASS timing: 100000 averages, remaining time, saturation, bounded backoff\n";
}

static void QueueSelectionAndAccounting() {
    DeviceState state;
    Scheduler scheduler(state);
    auto candidate=std::make_shared<type::KThread>();
    candidate->affinityMask=3;
    auto a=std::make_shared<type::KThread>(), b=std::make_shared<type::KThread>();
    a->averageTimeslice=1000; a->timesliceStart=20900;
    b->averageTimeslice=400; b->timesliceStart=20900;
    scheduler.cores[0].queue={a}; scheduler.cores[1].queue={b};
    assert(scheduler.GetOptimalCoreForThread(candidate).id==1);
    b->averageTimeslice=1000;
    assert(scheduler.GetOptimalCoreForThread(candidate).id==0); // Prefer no migration on a tie.
    scheduler.cores[1].queue.clear();
    assert(scheduler.GetOptimalCoreForThread(candidate).id==1);
    candidate->affinityMask=1;
    assert(scheduler.GetOptimalCoreForThread(candidate).id==0);
    state.thread=a;
    a->averageTimeslice=80;
    scheduler.cores[0].queue={a,b};
    scheduler.Rotate();
    assert(a->averageTimeslice==95);
    assert(scheduler.cores[0].queue.front()==b);
    state.thread=b; b->averageTimeslice=80;
    scheduler.RemoveThread();
    assert(b->averageTimeslice==95);
    assert(scheduler.cores[0].queue.front()==a);
    std::cout << "PASS production methods: selection, affinities, rotation, removal\n";
}

static void WaitsAndMigration() {
    DeviceState state;
    Scheduler scheduler(state);
    auto waiter=std::make_shared<type::KThread>();
    auto owner=std::make_shared<type::KThread>();
    state.thread=waiter;
    scheduler.cores[0].queue={owner,waiter};
    auto begin=std::chrono::steady_clock::now();
    assert(!scheduler.TimedWaitSchedule(20ms));
    assert(std::chrono::steady_clock::now()-begin >= 15ms);
    assert(!scheduler.TimedWaitSchedule(0ns));
    scheduler.cores[0].queue={waiter};
    waiter->scheduleCondition.notify_one(); // Notification before wait must not lose readiness.
    assert(scheduler.TimedWaitSchedule(0ns));
    scheduler.WaitSchedule(false);

    // Exercise wakeFunction's lock reassignment from an invalid to an allowed core.
    waiter->affinityMask=2; waiter->idealCore=1;
    assert(scheduler.TimedWaitSchedule(50ms));
    assert(waiter->coreId==1 && scheduler.cores[0].queue.empty());
    assert(scheduler.cores[1].queue.front()==waiter);

    // An eligible idle core must be selected after a blocking load-balance timeout.
    waiter->affinityMask=3; waiter->coreId=0;
    scheduler.cores[1].queue.clear();
    scheduler.cores[0].queue={owner,waiter};
    scheduler.WaitSchedule(true);
    assert(waiter->coreId==1);
    assert(scheduler.cores[0].queue.front()==owner);
    assert(scheduler.cores[1].queue.front()==waiter);
    std::cout << "PASS production waits: timeout, prior notification, affinity migration, idle core\n";
}

static double ThreadCpuSeconds() {
    timespec now{};
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now)==0);
    return now.tv_sec+now.tv_nsec/1e9;
}

static void WakeStressAndIdleCpu() {
    DeviceState state;
    Scheduler scheduler(state);
    auto waiter=std::make_shared<type::KThread>();
    auto owner=std::make_shared<type::KThread>();
    scheduler.cores[0].queue={owner,waiter};
    std::atomic<int> completed{};
    std::atomic<bool> started{};
    std::thread worker([&] {
        state.thread=waiter;
        started=true;
        auto cpu=ThreadCpuSeconds();
        scheduler.WaitSchedule(false);
        cpu=ThreadCpuSeconds()-cpu;
        std::cout << "MEASURE idle waiter CPU over 120ms: " << cpu*1000 << " ms\n";
        assert(cpu < 0.025); // Generous host-only bound, not an Android performance claim.
        for (int i=1; i<=2000; ++i) {
            {
                std::scoped_lock lock(scheduler.cores[0].mutex);
                scheduler.cores[0].queue={owner,waiter};
                completed=i;
            }
            scheduler.WaitSchedule(false);
        }
    });
    while(!started) std::this_thread::yield();
    // Notifications without a changed predicate must not schedule the waiter.
    for(int i=0;i<12;++i) {
        waiter->scheduleCondition.notify_one();
        std::this_thread::sleep_for(10ms);
        assert(completed==0);
    }
    {
        std::scoped_lock lock(scheduler.cores[0].mutex);
        scheduler.cores[0].queue={waiter,owner};
        waiter->scheduleCondition.notify_one();
    }
    for(int i=1;i<=2000;++i) {
        while(completed<i) std::this_thread::yield();
        std::scoped_lock lock(scheduler.cores[0].mutex);
        scheduler.cores[0].queue={waiter,owner};
        waiter->scheduleCondition.notify_one();
    }
    worker.join();
    assert(completed==2000);
    std::cout << "PASS production WaitSchedule: 2000 wake handoffs and unchanged predicates\n";
}

int main() {
    Timing();
    QueueSelectionAndAccounting();
    WaitsAndMigration();
    WakeStressAndIdleCpu();
    std::cout << "PASS all scheduler host regressions (UBSan enabled)\n";
}
