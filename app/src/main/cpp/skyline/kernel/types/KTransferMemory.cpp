// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KTransferMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KTransferMemory::KTransferMemory(const DeviceState &state, size_t size)
        : KMemory{state, KType::KTransferMemory, size} {}

    u8 *KTransferMemory::Map(span<u8> map, memory::Permission permission) {
        auto &manager{state.process->memory};
        std::unique_lock lock{manager.mutex};
        if (guest.valid() || map.size() != host.size() || !util::IsPageAligned(map.data()) ||
            !util::IsPageAligned(map.size()) || !manager.CheckRangeLocked(map, [](const ChunkDescriptor &chunk) {
                return chunk.state.transferMemoryAllowed && !chunk.attributes.value &&
                    chunk.permission == memory::Permission{true, true, false};
            })) {
            LOGW("Transfer memory source rejected: {} (0x{:X})", fmt::ptr(map.data()), map.size());
            return nullptr;
        }
        auto originals{manager.SnapshotRangeLocked(map)};
        auto hostMap{manager.GetHostSpan(map)};
        std::memcpy(host.data(), hostMap.data(), map.size());
        // If mmap fails, the source descriptors and source mapping are unchanged.
        u8 *result{KMemory::Map(map, permission)};
        originalMappings = std::move(originals);
        try {
            ChunkDescriptor descriptor{};
            descriptor.size = map.size();
            descriptor.permission = permission;
            descriptor.state = permission.raw ? memory::states::TransferMemory : memory::states::TransferMemoryIsolated;
            descriptor.attributes.isBorrowed = true;
            descriptor.isSrcMergeDisallowed = true;
            manager.MapInternal({map.data(), descriptor}, false);
        } catch (...) {
            LOGE("Failed to publish transfer memory mapping");
            std::terminate();
        }
        return result;
    }

    void KTransferMemory::Unmap(span<u8> map) {
        auto &manager{state.process->memory};
        std::unique_lock lock{manager.mutex};
        KMemory::Unmap(map); // Throws before descriptor changes if host remapping fails.
        try {
            auto hostMap{manager.GetHostSpan(map)};
            std::memcpy(hostMap.data(), host.data(), map.size());
            for (const auto &original : originalMappings)
                manager.MapInternal(original, false);
        } catch (...) {
            LOGE("Failed to restore transfer memory descriptors");
            std::terminate();
        }
        guest = skyline::span<u8>{nullptr};
        originalMappings.clear();
    }

    KTransferMemory::~KTransferMemory() {
        if (state.process && guest.valid()) {
            try {
                Unmap(guest);
            } catch (...) {
                // Continuing would leave mappings attached to a dead owner.
                LOGE("Failed to release transfer memory; stopping to preserve VMM integrity");
                std::terminate();
            }
        }
    }
}
