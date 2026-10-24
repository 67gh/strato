// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2024 Strato Team and Contributors (https://github.com/strato-emu/)

#include <sys/mman.h>
#include <cerrno>
#include <unistd.h>
#include <cstdlib>
#include "trace.h"
#include "trap_manager.h"

namespace skyline {
    // No formatting, allocation, or logger lock on this fatal path: it can run
    // from a fault callback. Partial mprotect success cannot safely be ignored.
    [[noreturn]] static void TrapProtectionFailure() noexcept {
        constexpr char message[]{"Strato: mprotect failed in TrapManager; aborting inconsistent protection state\n"};
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        std::abort();
    }

    static bool SetTrapProtection(void *address, size_t size, int protection) noexcept {
        int result;
        do {
            result = mprotect(address, size, protection);
        } while (result == -1 && errno == EINTR);
        return result == 0;
    }

    CallbackEntry::CallbackEntry(TrapProtection protection, LockCallback lockCallback, TrapCallback readCallback, TrapCallback writeCallback) : protection{protection}, lockCallback{std::move(lockCallback)}, readCallback{std::move(readCallback)}, writeCallback{std::move(writeCallback)} {}

    constexpr TrapHandle::TrapHandle(const TrapMap::GroupHandle &handle) : TrapMap::GroupHandle(handle) {}

    TrapHandle TrapManager::CreateTrap(span<span<u8>> regions, const LockCallback &lockCallback, const TrapCallback &readCallback, const TrapCallback &writeCallback) {
        TRACE_EVENT("host", "TrapManager::CreateTrap");
        std::scoped_lock lock{trapMutex};
        TrapHandle handle{trapMap.Insert(regions, CallbackEntry{TrapProtection::None, lockCallback, readCallback, writeCallback})};
        return handle;
    }

    void TrapManager::TrapRegions(TrapHandle handle, bool writeOnly) {
        TRACE_EVENT("host", "TrapManager::TrapRegions");
        std::scoped_lock lock{trapMutex};
        auto protection{writeOnly ? TrapProtection::WriteOnly : TrapProtection::ReadWrite};
        handle->value.protection = protection;
        ReprotectIntervals(handle->intervals, protection);
    }

    void TrapManager::RemoveTrap(TrapHandle handle) {
        TRACE_EVENT("host", "TrapManager::RemoveTrap");
        std::scoped_lock lock{trapMutex};
        handle->value.protection = TrapProtection::None;
        ReprotectIntervals(handle->intervals, TrapProtection::None);
    }

    void TrapManager::DeleteTrap(TrapHandle handle) {
        TRACE_EVENT("host", "TrapManager::DeleteTrap");
        std::scoped_lock lock{trapMutex};
        handle->value.protection = TrapProtection::None;
        ReprotectIntervals(handle->intervals, TrapProtection::None);
        trapMap.Remove(handle);
    }

    void TrapManager::ReprotectIntervals(const std::vector<TrapMap::Interval> &intervals, TrapProtection protection) {
        TRACE_EVENT("host", "TrapManager::ReprotectIntervals");

        auto reprotectIntervalsWithFunction = [&intervals](auto getProtection) {
            for (auto region : intervals) {
                region = region.Align(constant::PageSize);
                if (!SetTrapProtection(region.start, region.Size(), getProtection(region)))
                    TrapProtectionFailure();
            }
        };

        // We need to determine the lowest protection possible for the given interval
        switch (protection) {
            case TrapProtection::None:
                reprotectIntervalsWithFunction([&](auto region) {
                    auto entries{trapMap.GetRange(region)};

                    TrapProtection lowestProtection{TrapProtection::None};
                    for (const auto &entry : entries) {
                        auto entryProtection{entry.get().protection};
                        if (entryProtection > lowestProtection) {
                            lowestProtection = entryProtection;
                            if (entryProtection == TrapProtection::ReadWrite)
                                return PROT_NONE;
                        }
                    }

                    switch (lowestProtection) {
                        case TrapProtection::None:
                            return PROT_READ | PROT_WRITE | PROT_EXEC;
                        case TrapProtection::WriteOnly:
                            return PROT_READ | PROT_EXEC;
                        case TrapProtection::ReadWrite:
                            return PROT_NONE;
                    }
                });
                break;

            case TrapProtection::WriteOnly:
                reprotectIntervalsWithFunction([&](auto region) {
                    auto entries{trapMap.GetRange(region)};
                    for (const auto &entry : entries)
                        if (entry.get().protection == TrapProtection::ReadWrite)
                            return PROT_NONE;

                    return PROT_READ | PROT_EXEC;
                });
                break;

            case TrapProtection::ReadWrite:
                reprotectIntervalsWithFunction([&](auto region) {
                    return PROT_NONE; // No checks are needed as this is already the highest level of protection
                });
                break;
        }
    }

    static TrapManager *staticTrap{nullptr};

    void TrapManager::InstallStaticInstance() {
        staticTrap = this;
    }

    bool TrapManager::TrapHandler(u8 *address, bool write) {
        assert(staticTrap != nullptr);
        return staticTrap->HandleTrap(address, write);
    }

    bool TrapManager::HandleTrap(u8 *address, bool write) {
        TRACE_EVENT("host", "TrapManager::TrapHandler");

        LockCallback lockCallback{};
        while (true) {
            if (lockCallback) {
                // We want to avoid a deadlock of holding trapMutex while locking the resource inside a callback while another thread holding the resource's mutex waits on trapMutex, we solve this by quitting the loop if a callback would be blocking and attempt to lock the resource externally
                lockCallback();
                lockCallback = {};
            }

            std::scoped_lock lock(trapMutex);

            // Retrieve any callbacks for the page that was faulted
            auto [entries, intervals]{trapMap.GetAlignedRecursiveRange<constant::PageSize>(address)};
            if (entries.empty())
                return false; // There's no callbacks associated with this page

            // Do callbacks for every entry in the intervals
            if (write) {
                for (auto entryRef : entries) {
                    auto &entry{entryRef.get()};
                    if (entry.protection == TrapProtection::None)
                        // We don't need to do the callback if the entry doesn't require any protection already
                        continue;

                    if (!entry.writeCallback()) {
                        lockCallback = entry.lockCallback;
                        break;
                    }
                    entry.protection = TrapProtection::None; // We don't need to protect this entry anymore
                }
                if (lockCallback)
                    continue; // We need to retry the loop because a callback was blocking
            } else {
                bool allNone{true}; // If all entries require no protection, we can protect to allow all accesses
                for (auto entryRef : entries) {
                    auto &entry{entryRef.get()};
                    if (entry.protection < TrapProtection::ReadWrite) {
                        // We don't need to do the callback if the entry can already handle read accesses
                        allNone = allNone && entry.protection == TrapProtection::None;
                        continue;
                    }

                    if (!entry.readCallback()) {
                        lockCallback = entry.lockCallback;
                        break;
                    }
                    entry.protection = TrapProtection::WriteOnly; // We only need to trap writes to this entry
                    allNone = false; // A read callback must not silently remove the pending write trap.
                }
                if (lockCallback)
                    continue; // We need to retry the loop because a callback was blocking
                write = allNone;
            }

            int permission{PROT_READ | (write ? PROT_WRITE : 0) | PROT_EXEC};
            for (const auto &interval : intervals)
                // Reprotect the interval to the lowest protection level that the callbacks performed allow
                if (!SetTrapProtection(interval.start, interval.Size(), permission))
                    TrapProtectionFailure();

            return true;
        }
    }
}

