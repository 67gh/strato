// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KSharedMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KSharedMemory::KSharedMemory(const DeviceState &state, size_t size)
        : KMemory{state, KType::KSharedMemory, size} {}

    u8 *KSharedMemory::Map(span<u8> map, memory::Permission permission) {
        auto &memory{state.process->memory};
        std::unique_lock lock{memory.mutex};
        if (guest.valid() || !memory.CheckRangeLocked(map, [](const ChunkDescriptor &chunk) {
                return chunk.state == memory::states::Unmapped && !chunk.attributes.value;
            }))
            return nullptr;
        u8 *result{KMemory::Map(map, permission)};

        // mmap already installed the host protection. Do not re-enter the VMM lock.
        try {
            ChunkDescriptor descriptor{};
            descriptor.size = guest.size();
            descriptor.permission = permission;
            descriptor.state = memory::states::SharedMemory;
            descriptor.isSrcMergeDisallowed = true;
            memory.MapInternal({guest.data(), descriptor}, false);
        } catch (...) {
            // The host mapping has changed: never continue with stale descriptors.
            LOGE("Failed to publish shared memory mapping");
            std::terminate();
        }

        return result;
    }

    void KSharedMemory::Unmap(span<u8> map) {
        auto &manager{state.process->memory};
        std::unique_lock lock{manager.mutex};
        KMemory::Unmap(map);
        try {
            ChunkDescriptor descriptor{};
            descriptor.size = map.size();
            descriptor.state = memory::states::Unmapped;
            manager.MapInternal({map.data(), descriptor});
        } catch (...) {
            LOGE("Failed to publish shared memory unmap");
            std::terminate();
        }
        guest = {};
    }

    KSharedMemory::~KSharedMemory() {
        if (state.process && guest.valid()) {
            try {
                Unmap(guest);
            } catch (...) {
                LOGE("Failed to release shared memory; stopping to preserve VMM integrity");
                std::terminate();
            }
        }
    }
}
