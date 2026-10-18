
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
// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)



namespace skyline {
    /**
     * @brief A singly-linked list with atomic access to allow for lock-free access semantics
     */
    template<typename Type>
    class AtomicForwardList {
      private:
        struct Node {
            Node *next;
            Type value;
        };

        std::atomic<Node *> head{}; //!< The head of the list

      public:
        AtomicForwardList() = default;

        AtomicForwardList(const AtomicForwardList &) = delete;

        AtomicForwardList(AtomicForwardList &&other) {
            head = other.head.load();
            while (!other.head.compare_exchange_strong(head, nullptr, std::memory_order_release, std::memory_order_consume));
        }

        ~AtomicForwardList() {
            Clear();
        }

        /**
         * @brief Clears all the items from the list while deallocating them
         */
        void Clear() {
            auto current{head.exchange(nullptr, std::memory_order_acquire)};
            while (current) {
                auto next{current->next};
                delete current;
                current = next;
            }
        }

        /**
         * @brief Appends an item to the start of the list
         */
        void Append(Type item) {
            auto node{new Node{nullptr, item}};
            auto next{head.load(std::memory_order_consume)};
            do {
                node->next = next;
            } while (!head.compare_exchange_strong(next, node, std::memory_order_release, std::memory_order_consume));
        }

        /**
         * @brief Appends multiple items to the start of the list
         */
        void Append(std::initializer_list<Type> items) {
            if (std::empty(items))
                return;

            Node* firstNode{new Node{nullptr, *items.begin()}};
            Node* lastNode{firstNode};
            for (auto item{items.begin() + 1}; item != items.end(); item++)
                lastNode = new Node{lastNode, *item};

            auto next{head.load(std::memory_order_consume)};
            do {
                firstNode->next = next;
            } while (!head.compare_exchange_strong(next, lastNode, std::memory_order_release, std::memory_order_consume));
        }

        template<typename... Items>
        void Append(Items &&... items) {
            Append(std::initializer_list<Type>{std::forward<Items>(items)...});
        }

        /**
         * @brief Iterates over every single list item and calls the given function
         * @note This function is **not** thread-safe when used with Clear() as the item may be deallocated while iterating
         */
        template <typename Function>
        void Iterate(Function function) {
            auto current{head.load(std::memory_order_consume)};
            while (current) {
                function(current->value);
                current = current->next;
            }
        }

        /**
         * @brief Iterates over every single list item and calls the given function till the given predicate returns false
         * @note This function is **not** thread-safe when used with Clear() as the item may be deallocated while iterating
         */
        template <typename Function>
        bool AllOf(Function function) {
            auto current{head.load(std::memory_order_consume)};
            while (current) {
                if (!function(current->value))
                    return false;
                current = current->next;
            }
            return true;
        }
    };
}
// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)



namespace skyline::gpu {
    class CommandScheduler;

    /**
     * @brief A wrapper around a Vulkan Fence which only tracks a single reset -> signal cycle with the ability to attach lifetimes of objects to it
     * @note This provides the guarantee that the fence must be signalled prior to destruction when objects are to be destroyed
     * @note All waits to the fence **must** be done through the same instance of this, the state of the fence changing externally will lead to UB
     */
    struct FenceCycle {
      private:
        std::atomic_flag diagnosticCompleted{};
        void ReportCompletion() noexcept {
            if (!diagnosticCompleted.test_and_set()) skyline::diagnostics::Completed(diagnosticId);
        }
        std::atomic_flag signalled{}; //!< If the underlying fence has been signalled since the creation of this FenceCycle, this doesn't necessarily mean the dependencies have been destroyed
        std::atomic_flag alreadyDestroyed{}; //!< If the cycle's dependencies are already destroyed, this prevents multiple destructions
        const vk::raii::Device &device;
        std::recursive_timed_mutex mutex;
        std::condition_variable_any submitCondition;
        bool submitted{}; //!< If the fence has been submitted to the GPU
        vk::Fence fence;
        vk::Semaphore semaphore; //!< Semaphore that will be signalled upon GPU completion of the fence
        bool semaphoreSubmitWait{}; //!< If the semaphore needs to be waited on (on GPU) before the fence's command buffer begins. Used to ensure fences that wouldn't otherwise be unsignalled are unsignalled
        bool nextSemaphoreSubmitWait{true}; //!< If the next fence cycle created from this one after it's signalled should wait on the semaphore to unsignal it
        std::shared_ptr<FenceCycle> semaphoreUnsignalCycle{}; //!< If the semaphore is used on the GPU, the cycle for the submission that uses it, so it can be waited on before the fence is signalled to ensure the semaphore is unsignalled

        friend CommandScheduler;

        AtomicForwardList<std::shared_ptr<void>> dependencies; //!< A list of all dependencies on this fence cycle
        AtomicForwardList<std::shared_ptr<FenceCycle>> chainedCycles; //!< A list of all chained FenceCycles, this is used to express multi-fence dependencies
        SharedSpinLock chainMutex;

        /**
         * @brief Destroy all the dependencies of this cycle
         */
        void DestroyDependencies() {
            if (!alreadyDestroyed.test_and_set(std::memory_order_release)) {
                dependencies.Clear();
                semaphoreUnsignalCycle = {};
                std::scoped_lock lock{chainMutex};
                chainedCycles.Clear();
            }
        }

      public:
        const skyline::diagnostics::Id diagnosticId{skyline::diagnostics::NewId()};
        FenceCycle(const vk::raii::Device &device, vk::Fence fence, vk::Semaphore semaphore, bool signalled = false) : signalled{signalled}, device{device}, fence{fence}, semaphore{semaphore}, nextSemaphoreSubmitWait{!signalled} {
            if (!signalled)
                device.resetFences(fence);
        }

        explicit FenceCycle(const FenceCycle &cycle) : signalled{false}, device{cycle.device}, fence{cycle.fence}, semaphore{cycle.semaphore}, semaphoreSubmitWait{cycle.nextSemaphoreSubmitWait} {
            device.resetFences(fence);
        }

        ~FenceCycle() noexcept {
            try {
                Wait();
            } catch (const std::exception &e) {
                // Explicit waits still propagate errors; destruction must not throw.
                try { LOGE("Fence destruction after GPU failure: {}", e.what()); } catch (...) {}
            } catch (...) {
                try { LOGE("Fence destruction after unknown GPU failure"); } catch (...) {}
            }
        }

        /**
         * @brief Cancels unsubmitted work; submitted work must drain before resources can be released
         */
        void Cancel() {
            {
                std::scoped_lock lock{mutex};
                // Cancellation is safe only before a queue owns the resources.
                if (submitted) {
                    skyline::diagnostics::Event("fence_cancel_deferred", diagnosticId);
                    return;
                }
                skyline::diagnostics::Event("fence_cancelled", diagnosticId);
                signalled.test_and_set(std::memory_order_release);
            }
            submitCondition.notify_all();
            DestroyDependencies();
        }

        /**
         * @brief Executes a function with the fence locked to record a usage of its semaphore, if no semaphore can be provided then a CPU-side wait will be performed instead
         */
        std::shared_ptr<FenceCycle> RecordSemaphoreWaitUsage(std::function<std::shared_ptr<FenceCycle>(vk::Semaphore sema)> &&func) {
            // We can't submit any semaphore waits until the signal has been submitted, so do that first
            WaitSubmit();

            std::unique_lock lock{mutex};

            // If we already have a semaphore usage, just wait on the fence since we can't wait on it twice and have no way to add one after the fact
            if (semaphoreUnsignalCycle) {
                // Safe to unlock since semaphoreUnsignalCycle can never be reset
                lock.unlock();

                Wait();
                return func({});
            }

            // If we're already signalled then there's no need to wait on the semaphore
            if (signalled.test(std::memory_order_relaxed))
                return func({});

            semaphoreUnsignalCycle = func(semaphore);
            nextSemaphoreSubmitWait = false; // We don't need a semaphore wait on the next fence cycle to unsignal the semaphore anymore as the usage will do that
            return semaphoreUnsignalCycle;
        }

        /**
         * @brief Waits for submission of the command buffer associated with this cycle to the GPU
         */
        void WaitSubmit() {
            if (signalled.test(std::memory_order_consume))
                return;

            std::unique_lock lock{mutex};
            if (submitted)
                return;

            if (signalled.test(std::memory_order_consume))
                return;

            lock.unlock();
            {
                std::shared_lock chainLock{chainMutex};

                chainedCycles.Iterate([&](const auto &cycle) {
                    cycle->WaitSubmit();
                });
            }
            lock.lock();

            submitCondition.wait(lock, [this] { return submitted || signalled.test(std::memory_order_acquire); });
        }

        /**
         * @brief Wait on a fence cycle till it has been signalled
         * @param shouldDestroy If true, the dependencies of this cycle will be destroyed after the fence is signalled
         */
        void Wait(bool shouldDestroy = false) {
            if (signalled.test(std::memory_order_consume)) {
                if (shouldDestroy) {
                    std::unique_lock lock{mutex};
                    DestroyDependencies();
                }
                return;
            }

            {
                std::shared_lock lock{chainMutex};
                chainedCycles.Iterate([shouldDestroy](auto &cycle) {
                    cycle->Wait(shouldDestroy);
                });
            }

            std::unique_lock lock{mutex};

            skyline::diagnostics::Event("fence_wait_begin", diagnosticId);
            submitCondition.wait(lock, [&] { return submitted || signalled.test(std::memory_order_acquire); });

            if (signalled.test(std::memory_order_relaxed)) {
                if (shouldDestroy)
                    DestroyDependencies();
                return;
            }

            vk::Result waitResult;
            while ((waitResult = (*device).waitForFences(1, &fence, false, 50'000'000ULL, *device.getDispatcher())) != vk::Result::eSuccess) {
                // Let status/teardown operations observe the cycle between driver waits.
                lock.unlock();
                std::this_thread::yield();
                lock.lock();
                // Another waiter may have completed this cycle while unlocked;
                // its fence may already have been reset for the next cycle.
                if (signalled.test(std::memory_order_acquire)) {
                    if (shouldDestroy) DestroyDependencies();
                    return;
                }
                if (waitResult == vk::Result::eTimeout)
                    // Retry if the waiting time out
                    continue;

                if (waitResult == vk::Result::eErrorInitializationFailed)
                    // eErrorInitializationFailed occurs on Mali GPU drivers due to them using the ppoll() syscall which isn't correctly restarted after a signal, we need to manually retry waiting in that case
                    continue;

                if (waitResult == vk::Result::eErrorDeviceLost) diagnostics::CaptureDeviceFault(static_cast<VkDevice>(*device));
                diagnostics::DumpFaultReport(fmt::format("waiting for a fence failed: {}", vk::to_string(waitResult)));
                throw exception("An error occurred while waiting for fence {}: {}", fmt::ptr(static_cast<VkFence>(fence)), vk::to_string(waitResult));
            }

            ReportCompletion();
            if (semaphoreUnsignalCycle)
                semaphoreUnsignalCycle->Wait();

            signalled.test_and_set(std::memory_order_relaxed);
            if (shouldDestroy)
                DestroyDependencies();
        }

        /**
         * @param quick Skips the call to check the fence's status, just checking the signalled flag
         * @return If the fence is signalled currently or not
         */
        bool Poll(bool quick = true, bool shouldDestroy = false) {
            if (signalled.test(std::memory_order_consume)) {
                if (shouldDestroy) {
                    std::unique_lock lock{mutex, std::try_to_lock};
                    if (!lock)
                        return false;

                    DestroyDependencies();
                }
                return true;
            }

            if (quick)
                return false; // We need to return early if we're not waiting on the fence

            {
                std::shared_lock lock{chainMutex, std::try_to_lock};
                if (!lock)
                    return false;

                if (!chainedCycles.AllOf([=](auto &cycle) { return cycle->Poll(quick, shouldDestroy); }))
                    return false;
            }

            std::unique_lock lock{mutex, std::try_to_lock};
            if (!lock)
                return false;

            if (signalled.test(std::memory_order_relaxed)) {
                if (shouldDestroy)
                    DestroyDependencies();
                return true;
            }

            if (!submitted)
                return false;

            auto status{(*device).getFenceStatus(fence, *device.getDispatcher())};
            if (status == vk::Result::eSuccess) {
                ReportCompletion();
                if (semaphoreUnsignalCycle && !semaphoreUnsignalCycle->Poll())
                    return false;

                signalled.test_and_set(std::memory_order_relaxed);
                if (shouldDestroy)
                    DestroyDependencies();
                return true;
            } else {
                return false;
            }
        }

        /**
         * @brief Attach the lifetime of an object to the fence being signalled
         */
        void AttachObject(const std::shared_ptr<void> &dependency) {
            if (!signalled.test(std::memory_order_consume))
                dependencies.Append(dependency);
        }

        /**
         * @brief A version of AttachObject optimized for several objects being attached at once
         */
        template<typename... Dependencies>
        void AttachObjects(Dependencies &&... pDependencies) {
            if (!signalled.test(std::memory_order_consume))
                dependencies.Append(pDependencies...);
        }

        /**
         * @brief Chains another cycle to this cycle, this cycle will not be signalled till the supplied cycle is signalled
         * @param cycle The cycle to chain to this one, this is nullable and this function will be a no-op if this is nullptr
         */
        void ChainCycle(const std::shared_ptr<FenceCycle> &cycle) {
            if (cycle && !signalled.test(std::memory_order_consume) && cycle.get() != this && !cycle->Poll()) {
                std::shared_lock lock{chainMutex};
                skyline::diagnostics::Event("fence_dependency", diagnosticId, cycle->diagnosticId);
                chainedCycles.Append(cycle); // If the cycle isn't the current cycle or already signalled, we need to chain it
            }
        }

        /**
         * @brief Notifies all waiters that the command buffer associated with this cycle has been submitted
         */
        void NotifySubmitted() {
            std::scoped_lock lock{mutex};
            submitted = true;
            skyline::diagnostics::Submitted(diagnosticId);
            submitCondition.notify_all();
        }
    };
}
namespace skyline {
struct FakeSettings {bool setting{};u32 scale{};bool* forceMaxGpuClocks{&setting};u32* executorSlotCountScale{&scale};};
struct FakeProcess {std::atomic<unsigned> killed{};void Kill(bool){++killed;}};
struct DeviceState {std::shared_ptr<FakeSettings> settings{std::make_shared<FakeSettings>()};std::shared_ptr<FakeProcess> process{std::make_shared<FakeProcess>()};};
}
inline void adrenotools_set_turbo(bool){}
namespace skyline::gpu::interconnect {
class ExecutionWaiterThread {
      private:
        const DeviceState &state;
        SpinLock mutex;
        std::condition_variable_any condition;
        std::queue<std::pair<std::shared_ptr<FenceCycle>, std::function<void()>>> pendingSignalQueue; //!< Queue of callbacks to be executed when their coressponding fence is signalled
        std::atomic<bool> idle{};
        bool stopping{}; //!< Protected by mutex
        std::exception_ptr failure; //!< Protected by mutex
        std::thread thread; //!< Start only after all worker state has been constructed

        void Run();

      public:
        ExecutionWaiterThread(const DeviceState &state);
        ~ExecutionWaiterThread();
        void Stop();

        bool IsIdle() const;

        /**
         * @brief Queues `callback` to be executed when `cycle` is signalled, null values are valid for either, will null cycle representing an immediate callback (dep on previously queued cycles) and null callback representing a wait with no callback
         */
        void Queue(std::shared_ptr<FenceCycle> cycle, std::function<void()> &&callback);
    };
    void ExecutionWaiterThread::Run() {
        try {
            while (true) {
                std::pair<std::shared_ptr<FenceCycle>, std::function<void()>> item;
                {
                    std::unique_lock lock{mutex};
                    idle = pendingSignalQueue.empty();
                    if (idle && *state.settings->forceMaxGpuClocks)
                        adrenotools_set_turbo(false);
                    condition.wait(lock, [this] { return stopping || !pendingSignalQueue.empty(); });
                    if (pendingSignalQueue.empty()) break; // Stop drains already accepted work.
                    item = std::move(pendingSignalQueue.front());
                    pendingSignalQueue.pop();
                    idle = false;
                }
                if (*state.settings->forceMaxGpuClocks)
                    adrenotools_set_turbo(true);
                {
                    TRACE_EVENT("gpu", "GPU");
                    if (item.first) item.first->Wait();
                }
                if (item.second) item.second();
            }
        } catch (...) {
            auto error{std::current_exception()};
            std::queue<std::pair<std::shared_ptr<FenceCycle>, std::function<void()>>> abandoned;
            {
                std::scoped_lock lock{mutex};
                failure = error;
                stopping = true;
                // Discard callbacks; a completion promise becomes broken rather
                // than telling the guest that failed GPU work succeeded.
                abandoned.swap(pendingSignalQueue);
            }
            condition.notify_all();
            try {
                try { std::rethrow_exception(error); }
                catch (const std::exception &e) { LOGE("GPU completion worker failed: {}", e.what()); }
                catch (...) { LOGE("GPU completion worker failed with an unknown exception"); }
                if (state.process) state.process->Kill(false);
            } catch (...) {}
        }
        idle = true;
        if (*state.settings->forceMaxGpuClocks) adrenotools_set_turbo(false);
    }

    ExecutionWaiterThread::ExecutionWaiterThread(const DeviceState &state) : state{state}, thread{&ExecutionWaiterThread::Run, this} {}

    ExecutionWaiterThread::~ExecutionWaiterThread() { Stop(); }

    void ExecutionWaiterThread::Stop() {
        { std::scoped_lock lock{mutex}; stopping = true; }
        condition.notify_all();
        if (thread.joinable()) thread.join();
    }

    bool ExecutionWaiterThread::IsIdle() const { return idle; }

    void ExecutionWaiterThread::Queue(std::shared_ptr<FenceCycle> cycle, std::function<void()> &&callback) {
        {
            std::unique_lock lock{mutex};
            if (failure) std::rethrow_exception(failure);
            if (stopping) throw exception("GPU completion worker has stopped");
            pendingSignalQueue.push({std::move(cycle), std::move(callback)});
        }
        condition.notify_all();
    }


}
// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)



namespace skyline {
    /**
     * @brief An efficient consumer-producer oriented queue with internal synchronization
     */
    template<typename Type>
    class CircularQueue {
      private:
        std::allocator<Type> allocator;
        const size_t capacity;
        Type *storage; //!< Aligned uninitialized storage; only queued items are alive
        std::atomic<Type *> start; //!< Sentinel before the oldest item
        std::atomic<bool> closed{};
        std::atomic<Type *> end; //!< Newest item
        SpinLock consumptionMutex;
        std::condition_variable_any consumeCondition;
        SpinLock productionMutex;
        std::condition_variable_any produceCondition;

      public:
        /**
         * @note The internal allocation is an item larger as we require a sentinel value
         */
        CircularQueue(size_t size)
            : capacity{size && size < std::allocator_traits<std::allocator<Type>>::max_size(allocator)
                ? size + 1 : throw std::invalid_argument("Invalid circular queue capacity")},
              storage{allocator.allocate(capacity)}, start{storage}, end{storage} {}

        CircularQueue(const CircularQueue &) = delete;

        CircularQueue &operator=(const CircularQueue &) = delete;

        CircularQueue(CircularQueue &&) = delete;

        ~CircularQueue() {
            while (start != end) {
                auto next{start + 1};
                next = (next == storage + capacity) ? storage : next;
                std::destroy_at(next);
                start = next;
            }
            allocator.deallocate(storage, capacity);
        }

        /**
         * @return Whether the queue is empty
         */
        // Call before joining consumers; existing items are drained by Process.
        void Close() {
            { std::scoped_lock lock{productionMutex, consumptionMutex}; closed = true; }
            produceCondition.notify_all();
            consumeCondition.notify_all();
        }

        bool Empty() {
            return start == end;
        }

        /**
         * @brief A blocking for-each that runs on every item and waits till new items to run on them as well
         * @param function A function that is called for each item (with the only parameter as a reference to that item)
         * @param preWait An optional function that's called prior to waiting on more items to be queued
         */
        template<typename F1, typename F2>
        void Process(F1 function, F2 preWait) {
            TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");

            while (true) {
                if (start == end) {
                    std::unique_lock productionLock{productionMutex};
                    TRACE_EVENT_END("containers");
                    preWait();
                    produceCondition.wait(productionLock, [this]() { return closed || start != end; });
                    if (closed && start == end)
                        return;
                    TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");
                }

                std::scoped_lock comsumptionLock{consumptionMutex};
                while (start != end) {
                    auto next{start + 1};
                    next = (next == storage + capacity) ? storage : next;
                    function(*next);
                    std::destroy_at(next);
                    start = next;
                }

                consumeCondition.notify_all();
            }
        }

        Type Pop() {
            for (;;) {
                {
                    std::unique_lock productionLock{productionMutex};
                    produceCondition.wait(productionLock, [this]() { return closed || start != end; });
                    if (closed && start == end)
                        throw std::runtime_error("Pop on closed circular queue");
                }
                std::scoped_lock consumptionLock{consumptionMutex};
                if (start == end)
                    continue; // Another consumer won; never move an unconstructed slot.
                auto next{start + 1};
                next = (next == storage + capacity) ? storage : next;
                Type item{std::move(*next)};
                std::destroy_at(next);
                start = next;
                consumeCondition.notify_all();
                return item;
            }
        }

        void Push(const Type &item) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return closed || waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                if (closed)
                    throw std::runtime_error("Push on closed circular queue");
                auto next{end + 1};
                next = (next == storage + capacity) ? storage : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, item);
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        void Push(Type &&item) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return closed || waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                if (closed)
                    throw std::runtime_error("Push on closed circular queue");
                auto next{end + 1};
                next = (next == storage + capacity) ? storage : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, std::move(item));
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        template<typename... Args>
        void Emplace(Args &&... args) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return closed || waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                if (closed)
                    throw std::runtime_error("Push on closed circular queue");
                auto next{end + 1};
                next = (next == storage + capacity) ? storage : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, std::forward<Args>(args)...);
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        /**
         * @note The appended elements may not necessarily be directly contiguous as another thread could push elements in between those in the span
         */
        void Append(span<Type> buffer) {
            for (const auto &item : buffer)
                Push(item);
        }

        /**
         * @brief Appends a buffer with an alternative input type while supplied transformation function
         * @param tranformation A function that takes in an item of TransformedType as input and returns an item of Type
         */
        template<typename TransformedType, typename Transformation>
        void AppendTranform(TransformedType &container, Transformation transformation) {
            for (const auto &item : container)
                Push(transformation(item));
        }
    };
}
namespace skyline::constant {constexpr size_t NsInMillisecond=1000000;}
inline vk::raii::Device recordDevice;
inline std::atomic<bool> recordStarted{};
inline std::atomic<bool> blockRecordOutput{};
namespace skyline::gpu::interconnect {
class CommandRecordThread {
      public:
        /**
         * @brief Single execution slot, buffered back and forth between the GPFIFO thread and the record thread
         */
        struct Slot {
 std::mutex beginLock;std::condition_variable beginCondition;bool retired{};
 std::shared_ptr<FenceCycle> cycle{std::make_shared<FenceCycle>(recordDevice,vk::Fence{},vk::Semaphore{},true)};
};

      private:
        static constexpr size_t GrowThresholdNs{constant::NsInMillisecond / 50}; //!< The wait time threshold at which the slot count will be increased
        const DeviceState &state;
        CircularQueue<Slot *> incoming; //!< Slots pending recording
        CircularQueue<Slot *> outgoing; //!< Slots that have been submitted, may still be active on the GPU
        std::list<Slot> slots;
        std::atomic<bool> idle{};
        std::atomic<bool> stopping{};

        std::thread thread;

        void ProcessSlot(Slot *slot);

        void Run();

      public:
        CommandRecordThread(const DeviceState &state);
        ~CommandRecordThread();
        void Stop();

        bool IsIdle() const;

        /**
         * @return A free slot, `Reset` needs to be called before accessing it
         */
        Slot *AcquireSlot();

        /**
         * @brief Submit a slot to be recorded
         */
        void ReleaseSlot(Slot *slot);
    };
    CommandRecordThread::CommandRecordThread(const DeviceState &state)
        : state{state},
          incoming{1U << *state.settings->executorSlotCountScale},
          outgoing{1U << *state.settings->executorSlotCountScale},
          thread{&CommandRecordThread::Run, this} {}

CommandRecordThread::~CommandRecordThread() { Stop(); }
void CommandRecordThread::Stop() {
        stopping = true;
        // A recorder may be blocked publishing an outgoing slot while holding the
        // incoming consumer lock. Wake it before closing the incoming queue.
        outgoing.Close();
        incoming.Close();
        if (thread.joinable()) thread.join();
        for (auto &entry : slots) {
            {
                std::scoped_lock lock{entry.beginLock};
                entry.retired = true;
                entry.beginCondition.notify_all();
            }
            entry.cycle->Cancel(); // No-op for submitted work; never release GPU-owned objects.
            try { entry.cycle->Wait(true); }
            catch (const std::exception &e) { LOGE("GPU slot drain failed: {}", e.what()); }
        }
    }

void CommandRecordThread::Run(){
 try{
  outgoing.Push(&slots.emplace_back());recordStarted=true;
  if(blockRecordOutput)outgoing.Push(&slots.front());
  incoming.Process([](Slot*){},[]{});
 }catch(...){}
 outgoing.Close();incoming.Close();
}
}

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
 std::cout<<"PASS GPU host shims: pre-submit cancellation, submitted resource retention, device loss, worker startup/drain/repeated stop/callback failure/broken promise\n";
}
