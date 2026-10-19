// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <nce.h>
#include <os.h>
#include <jvm.h>
#include <common/trace.h>
#include <kernel/results.h>
#include "KProcess.h"

namespace skyline::kernel::type {
    KProcess::TlsPage::TlsPage(u8 *memory) : memory(memory) {}

    u8 *KProcess::TlsPage::ReserveSlot() {
        if (index == constant::TlsSlots)
            return nullptr;
        return memory + (constant::TlsSlotSize * index++);
    }

    KProcess::KProcess(const DeviceState &state) : memory(state), KSyncObject(state, KType::KProcess) {
        trap.InstallStaticInstance();
    }

    KProcess::~KProcess() {
        std::scoped_lock guard{threadMutex};
        disableThreadCreation = true;
        for (const auto &thread : threads)
            thread->Kill(true);
    }

    void KProcess::Kill(bool join, bool all, bool disableCreation) {
        LOGW("Killing {}{}KProcess{}", join ? "and joining " : "", all ? "all threads in " : "HOS-1 in ", disableCreation ? " with new thread creation disabled" : "");
        // disableCreation is set only when gracefully exiting, it being false means an exception/crash occurred
        if (!disableCreation)
            state.jvm->reportCrash();

        bool expected{false};
        if (!join && !alreadyKilled.compare_exchange_strong(expected, true))
            // If the process has already been killed and we don't want to wait for it to join then just instantly return rather than waiting on the mutex
            return;
        else
            alreadyKilled.store(true);

        std::scoped_lock guard{threadMutex};
        if (disableCreation)
            disableThreadCreation = true;
        if (all) {
            for (const auto &thread : threads)
                thread->Kill(join);
        } else if (!threads.empty()) {
            threads[0]->Kill(join);
        }
    }

    void KProcess::InitializeHeapTls() {
        constexpr size_t DefaultHeapSize{0x200000};
        memory.MapHeapMemory(span<u8>{state.process->memory.heap.guest.data(), DefaultHeapSize});
        memory.processHeapSize = DefaultHeapSize;
        tlsExceptionContext = AllocateTlsSlot();
    }

    u8 *KProcess::AllocateTlsSlot() {
        std::scoped_lock lock{tlsMutex};
        u8 *slot;
        for (auto &tlsPage : tlsPages)
            if ((slot = tlsPage->ReserveSlot()))
                return slot;

        bool isAllocated{};

        u8 *pageCandidate{state.process->memory.tlsIo.guest.data()};
        while (state.process->memory.tlsIo.guest.contains(span<u8>(pageCandidate, constant::PageSize))) {
            auto chunk = memory.GetChunk(pageCandidate);
            if (!chunk)
                break;

            if (chunk->second.state == memory::states::Unmapped) {
                memory.MapThreadLocalMemory(span<u8>{pageCandidate, constant::PageSize});
                isAllocated = true;
                break;
            } else {
                pageCandidate = chunk->first + chunk->second.size;
            }
        }

        if (!isAllocated)
            throw exception("Failed to find free memory for a tls slot!");

        auto tlsPage{std::make_shared<TlsPage>(pageCandidate)};
        tlsPages.push_back(tlsPage);
        return tlsPage->ReserveSlot();
    }

    std::shared_ptr<KThread> KProcess::CreateThread(void *entry, u64 argument, void *stackTop, std::optional<i8> priority, std::optional<u8> idealCore) {
        std::scoped_lock guard{threadMutex};
        if (disableThreadCreation)
            return nullptr;
        if (!stackTop && threads.empty()) { //!< Main thread stack is created by the kernel and owned by the process
            bool isAllocated{};

            u8 *pageCandidate{memory.stack.guest.data()};
            while (state.process->memory.stack.guest.contains(span<u8>(pageCandidate, state.process->npdm.meta.mainThreadStackSize))) {
                auto chunk{memory.GetChunk(pageCandidate)};
                if (!chunk)
                    break;

                if (chunk->second.state == memory::states::Unmapped && chunk->second.size >= state.process->npdm.meta.mainThreadStackSize) {
                    memory.MapStackMemory(span<u8>{pageCandidate, state.process->npdm.meta.mainThreadStackSize});
                    isAllocated = true;
                    break;
                } else {
                    pageCandidate = chunk->first + chunk->second.size;
                }
            }

            if (!isAllocated)
                throw exception("Failed to map main thread stack!");

            stackTop = pageCandidate + state.process->npdm.meta.mainThreadStackSize;
            mainThreadStack = span<u8>(pageCandidate, state.process->npdm.meta.mainThreadStackSize);
        }
        size_t tid{threads.size() + 1}; //!< The first thread is HOS-1 rather than HOS-0, this is to match the HOS kernel's behaviour
        auto thread{NewHandle<KThread>(this, tid, entry, argument, stackTop, priority ? *priority : state.process->npdm.meta.mainThreadPriority, idealCore ? *idealCore : state.process->npdm.meta.idealCore).item};
        threads.push_back(thread);
        return thread;
    }

    void KProcess::ClearHandleTable() {
        std::vector<std::shared_ptr<KObject>> retired;
        {
            std::unique_lock lock(handleMutex);
            retired.swap(handles);
        }
    }

    constexpr u32 HandleWaitersBit{1UL << 30}; //!< A bit which denotes if a mutex psuedo-handle has waiters or not

    Result KProcess::MutexLock(const std::shared_ptr<KThread> &thread, u32 *mutex, KHandle ownerHandle, KHandle tag, bool failOnOutdated) {
        TRACE_EVENT_FMT("kernel", "MutexLock {} @ 0x{:X}", fmt::ptr(mutex), thread->id);
        {
            std::scoped_lock lock{syncWaiterMutex};
            std::shared_ptr<KThread> owner;
            try {
                owner = GetHandle<KThread>(ownerHandle);
            } catch (const std::out_of_range &) {
                if (__atomic_load_n(mutex, __ATOMIC_SEQ_CST) != (ownerHandle | HandleWaitersBit))
                    return failOnOutdated ? result::InvalidCurrentMemory : Result{};
                return result::InvalidHandle;
            }
            if (__atomic_load_n(mutex, __ATOMIC_SEQ_CST) != (ownerHandle | HandleWaitersBit))
                return failOnOutdated ? result::InvalidCurrentMemory : Result{};

            auto &waiters{owner->waiters};
            waiters.insert(std::upper_bound(waiters.begin(), waiters.end(), thread->priority.load(), KThread::IsHigherPriority), thread);
            if (thread == state.thread)
                state.scheduler->RemoveThread();
            thread->waitThread = owner;
            thread->waitMutex = mutex;
            thread->waitTag = tag;
            owner->UpdatePriorityInheritance();
        }
        // Never sleep with the process wait graph locked.
        if (thread == state.thread)
            state.scheduler->WaitSchedule(false);
        return {};
    }

    void KProcess::MutexUnlock(u32 *mutex) {
        TRACE_EVENT_FMT("kernel", "MutexUnlock {}", fmt::ptr(mutex));
        std::scoped_lock lock{syncWaiterMutex};
        auto &waiters{state.thread->waiters};
        auto nextOwnerIt{std::find_if(waiters.begin(), waiters.end(), [mutex](const std::shared_ptr<KThread> &thread) { return thread->waitMutex == mutex; })};
        if (nextOwnerIt == waiters.end()) {
            __atomic_store_n(mutex, 0, __ATOMIC_SEQ_CST);
            return;
        }

        auto nextOwner{*nextOwnerIt};
        nextOwner->waitThread.reset();
        nextOwner->waitMutex = nullptr;
        bool hasWaiters{};
        for (auto it{waiters.erase(nextOwnerIt)}; it != waiters.end();) {
            auto current{it++}; // Only increment an iterator known not to be end().
            auto thread{*current};
            if (thread->waitMutex == mutex) {
                nextOwner->waiters.splice(std::upper_bound(nextOwner->waiters.begin(), nextOwner->waiters.end(), thread->priority.load(), KThread::IsHigherPriority), waiters, current);
                thread->waitThread = nextOwner;
                hasWaiters = true;
            }
        }
        state.thread->UpdatePriorityInheritance();
        nextOwner->UpdatePriorityInheritance();
        __atomic_store_n(mutex, nextOwner->waitTag | (hasWaiters ? HandleWaitersBit : 0), __ATOMIC_SEQ_CST);
        nextOwner->waitTag = 0;
        state.scheduler->InsertThread(nextOwner);
    }

    Result KProcess::ConditionVariableWait(u32 *key, u32 *mutex, KHandle tag, i64 timeout) {
        TRACE_EVENT_FMT("kernel", "ConditionVariableWait {} ({})", fmt::ptr(key), fmt::ptr(mutex));
        {
            std::scoped_lock lock{syncWaiterMutex};
            if (timeout == 0) {
                MutexUnlock(mutex);
                return result::TimedOut;
            }
            state.thread->waitThread.reset();
            state.thread->waitMutex = mutex;
            state.thread->waitTag = tag;
            state.thread->waitConditionVariable = key;
            state.thread->waitSignalled = false;
            state.thread->waitResult = {};
            auto queue{syncWaiters.equal_range(key)};
            syncWaiters.insert(std::upper_bound(queue.first, queue.second, state.thread->priority.load(), [](const i8 priority, const SyncWaiters::value_type &it) { return it.second->priority > priority; }), {key, state.thread});
            __atomic_store_n(key, true, __ATOMIC_SEQ_CST);
            state.scheduler->RemoveThread();
            MutexUnlock(mutex);
        }

        if (timeout > 0 && !state.scheduler->TimedWaitSchedule(std::chrono::nanoseconds(timeout))) {
            std::scoped_lock lock{syncWaiterMutex};
            // A completed signal/unlock already scheduled us; do not insert twice.
            const bool alreadyScheduled{state.thread->waitSignalled && !state.thread->waitThread};
            if (!alreadyScheduled) {
                auto queue{syncWaiters.equal_range(key)};
                auto it{std::find(queue.first, queue.second, SyncWaiters::value_type{key, state.thread})};
                if (it != queue.second)
                    syncWaiters.erase(it);
                if (auto owner{state.thread->waitThread}) {
                    owner->waiters.remove(state.thread);
                    state.thread->waitThread.reset();
                    owner->UpdatePriorityInheritance();
                }
                state.thread->waitConditionVariable = nullptr;
                state.thread->waitMutex = nullptr;
                state.thread->waitTag = 0;
                state.thread->waitSignalled = true;
                state.thread->waitResult = result::TimedOut;
                queue = syncWaiters.equal_range(key);
                if (queue.first == queue.second)
                    __atomic_store_n(key, false, __ATOMIC_SEQ_CST);
                state.scheduler->InsertThread(state.thread);
            }
        }
        state.scheduler->WaitSchedule(false);
        std::scoped_lock lock{syncWaiterMutex};
        return state.thread->waitResult;
    }

    void KProcess::ConditionVariableSignal(u32 *key, i32 amount) {
        TRACE_EVENT_FMT("kernel", "ConditionVariableSignal {}", fmt::ptr(key));
        std::scoped_lock lock{syncWaiterMutex};
        i32 remaining{amount};
        while (amount <= 0 || remaining > 0) {
            auto queue{syncWaiters.equal_range(key)};
            if (queue.first == queue.second)
                break;
            auto it{std::min_element(queue.first, queue.second, [](const SyncWaiters::value_type &lhs, const SyncWaiters::value_type &rhs) { return lhs.second->priority < rhs.second->priority; })};
            auto thread{it->second};
            syncWaiters.erase(it);
            if (amount > 0)
                --remaining;

            u32 *mutex{thread->waitMutex};
            KHandle tag{thread->waitTag};
            // Publish the complete result/state before any scheduler wakeup.
            thread->waitConditionVariable = nullptr;
            thread->waitSignalled = true;
            thread->waitResult = {};
            while (true) {
                KHandle value{};
                if (__atomic_compare_exchange_n(mutex, &value, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    thread->waitMutex = nullptr;
                    thread->waitTag = 0;
                    state.scheduler->InsertThread(thread);
                    break;
                }
                if ((value & HandleWaitersBit) == 0)
                    if (!__atomic_compare_exchange_n(mutex, &value, value | HandleWaitersBit, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                        continue;
                auto result{MutexLock(thread, mutex, value & ~HandleWaitersBit, tag, true)};
                if (result == result::InvalidCurrentMemory)
                    continue;
                if (result == result::InvalidHandle) {
                    thread->waitResult = result::InvalidState;
                    thread->waitMutex = nullptr;
                    thread->waitTag = 0;
                    state.scheduler->InsertThread(thread);
                } else if (result != Result{}) {
                    throw exception("Failed to lock mutex: 0x{:X}", result);
                }
                break;
            }
        }
        auto queue{syncWaiters.equal_range(key)};
        if (queue.first == queue.second)
            __atomic_store_n(key, false, __ATOMIC_SEQ_CST);
    }

    Result KProcess::WaitForAddress(u32 *address, u32 value, i64 timeout, ArbitrationType type) {
        TRACE_EVENT_FMT("kernel", "WaitForAddress {}", fmt::ptr(address));

        {
            std::scoped_lock lock{syncWaiterMutex};

            u32 userValue{__atomic_load_n(address, __ATOMIC_SEQ_CST)};
            switch (type) {
                case ArbitrationType::WaitIfLessThan:
                    if (userValue >= value) [[unlikely]]
                        return result::InvalidState;
                    break;

                case ArbitrationType::DecrementAndWaitIfLessThan: {
                    do {
                        if (value <= userValue) [[unlikely]] // We want to explicitly decrement **after** the check
                            return result::InvalidState;
                    } while (!__atomic_compare_exchange_n(address, &userValue, userValue - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
                    break;
                }

                case ArbitrationType::WaitIfEqual:
                    if (userValue != value) [[unlikely]]
                        return result::InvalidState;
                    break;
            }

            if (timeout == 0) [[unlikely]]
                return result::TimedOut;

            auto queue{syncWaiters.equal_range(address)};
            syncWaiters.insert(std::upper_bound(queue.first, queue.second, state.thread->priority.load(), [](const i8 priority, const SyncWaiters::value_type &it) { return it.second->priority > priority; }), {address, state.thread});

            state.scheduler->RemoveThread();
        }

        if (timeout > 0 && !state.scheduler->TimedWaitSchedule(std::chrono::nanoseconds(timeout))) {
            bool shouldWait{false};
            {
                std::scoped_lock lock{syncWaiterMutex};
                auto queue{syncWaiters.equal_range(address)};
                auto iterator{std::find(queue.first, queue.second, SyncWaiters::value_type{address, state.thread})};
                if (iterator != queue.second) {
                    // An address-arbiter key is application data, not a condition flag.
                    syncWaiters.erase(iterator);
                } else {
                    // If we didn't find the thread in the queue then it must have been signalled already and we should just wait
                    shouldWait = true;
                }
            }

            if (shouldWait) {
                state.scheduler->WaitSchedule(false);
                return {};
            }

            state.scheduler->InsertThread(state.thread);
            state.scheduler->WaitSchedule(false);

            return result::TimedOut;
        } else {
            state.scheduler->WaitSchedule(false);
        }

        return {};
    }

    Result KProcess::SignalToAddress(u32 *address, u32 value, i32 amount, SignalType type) {
        TRACE_EVENT_FMT("kernel", "SignalToAddress {}", fmt::ptr(address));

        std::scoped_lock lock{syncWaiterMutex};
        auto queue{syncWaiters.equal_range(address)};

        if (type != SignalType::Signal) {
            u32 newValue{value};
            if (type == SignalType::SignalAndIncrementIfEqual) {
                newValue++;
            } else if (type == SignalType::SignalAndModifyBasedOnWaitingThreadCountIfEqual) {
                if (amount <= 0) {
                    if (queue.first != queue.second)
                        newValue -= 2;
                    else
                        newValue++;
                } else {
                    if (queue.first != queue.second) {
                        i32 waiterCount{static_cast<i32>(std::distance(queue.first, queue.second))};
                        if (waiterCount < amount)
                            newValue--;
                    } else {
                        newValue++;
                    }
                }
            }

            if (!__atomic_compare_exchange_n(address, &value, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) [[unlikely]]
                return result::InvalidState;
        }

        i32 waiterCount{amount};
        boost::container::small_vector<SyncWaiters::iterator, 10> orderedThreads;
        for (auto it{queue.first}; it != queue.second; it++) {
            // While threads should generally be inserted in priority order, they may not always be due to the way the kernel handles priority updates
            // As a result, we need to create a second list of threads ordered by priority to ensure that we wake up the highest priority threads first
            auto thread{it->second};
            orderedThreads.insert(std::upper_bound(orderedThreads.begin(), orderedThreads.end(), thread->priority.load(), [](const i8 priority, const SyncWaiters::iterator &it) { return it->second->priority > priority; }), it);
        }

        for (auto &it : orderedThreads) {
            auto thread{it->second};

            syncWaiters.erase(it);
            state.scheduler->InsertThread(thread);

            if (amount > 0 && --waiterCount == 0)
                break;
        }

        return {};
    }
}
