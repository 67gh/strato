// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Optimized megabuffer: adaptive chunk size for MediaTek (low RAM), smarter recycling

#include <gpu.h>
#include "megabuffer.h"

namespace skyline::gpu {

    // =========================================================================
    // Chunk-size selection
    // =========================================================================
    // Original: a single hard-coded 25 MiB chunk.
    // Optimized: 8 MiB on Mali/MediaTek (typically ≤6 GB LPDDR5 shared with GPU),
    //            16 MiB on Qualcomm (has its own VRAM pool),
    //            25 MiB on everything else (desktop-class).

    static vk::DeviceSize ChooseMegaBufferChunkSize(const GPU &gpu) {
        switch (gpu.traits.vendorId) {
            case memory::kVendorARM:      return  8u * 1024u * 1024u; //  8 MiB — Mali (MediaTek)
            case memory::kVendorQualcomm: return 16u * 1024u * 1024u; // 16 MiB — Adreno
            default:                      return 25u * 1024u * 1024u; // 25 MiB — default
        }
    }

    // =========================================================================
    // MegaBufferChunk
    // =========================================================================

    MegaBufferChunk::MegaBufferChunk(GPU &gpu)
        : backing{gpu.memory.AllocateBuffer(ChooseMegaBufferChunkSize(gpu))},
          freeRegion{backing.subspan(PAGE_SIZE)} {}

    bool MegaBufferChunk::TryReset() {
        if (cycle && cycle->Poll(true)) {
            freeRegion = backing.subspan(PAGE_SIZE);
            cycle = nullptr;
            return true;
        }
        return cycle == nullptr;
    }

    vk::Buffer MegaBufferChunk::GetBacking() const {
        return backing.vkBuffer;
    }

    std::pair<vk::DeviceSize, span<u8>> MegaBufferChunk::Allocate(
            const std::shared_ptr<FenceCycle> &newCycle,
            vk::DeviceSize size, bool pageAlign) {

        if (pageAlign) {
            auto alignedFreeBase{util::AlignUp(
                static_cast<size_t>(freeRegion.data() - backing.data()), PAGE_SIZE)};
            freeRegion = backing.subspan(alignedFreeBase);
        }

        if (size > freeRegion.size())
            return {0, {}};

        if (cycle != newCycle) {
            newCycle->ChainCycle(cycle);
            cycle = newCycle;
        }

        auto resultSpan{freeRegion.subspan(0, size)};
        freeRegion = freeRegion.subspan(size);

        return {static_cast<vk::DeviceSize>(resultSpan.data() - backing.data()), resultSpan};
    }

    // =========================================================================
    // MegaBufferAllocator
    // =========================================================================

    MegaBufferAllocator::MegaBufferAllocator(GPU &gpu)
        : gpu{gpu}, activeChunk{chunks.emplace(chunks.end(), gpu)} {}

    MegaBufferAllocator::Allocation MegaBufferAllocator::Allocate(
            const std::shared_ptr<FenceCycle> &cycle,
            vk::DeviceSize size, bool pageAlign) {

        // Fast path: active chunk has space.
        if (auto allocation{activeChunk->Allocate(cycle, size, pageAlign)}; allocation.first)
            return {activeChunk->GetBacking(), allocation.first, allocation.second};

        // Scan existing chunks for a reset-able one.
        activeChunk = ranges::find_if(chunks, [&](auto &chunk) { return chunk.TryReset(); });
        if (activeChunk == chunks.end()) {
            // Limit pool growth: on Mali keep at most 3 chunks in flight
            // (= 3 × 8 MiB = 24 MiB max megabuffer RAM).
            constexpr size_t kMaliMaxChunks{3};
            if (gpu.traits.vendorId == memory::kVendorARM &&
                chunks.size() >= kMaliMaxChunks) {
                // Stall on the oldest chunk rather than OOM-ing.
                activeChunk = chunks.begin();
                activeChunk->TryReset(); // blocks until GPU signals
            } else {
                activeChunk = chunks.emplace(chunks.end(), gpu);
            }
        }

        if (auto allocation{activeChunk->Allocate(cycle, size, pageAlign)}; allocation.first)
            return {activeChunk->GetBacking(), allocation.first, allocation.second};
        else
            throw exception("Failed to allocate megabuffer space for size: 0x{:X}", size);
    }

    MegaBufferAllocator::Allocation MegaBufferAllocator::Push(
            const std::shared_ptr<FenceCycle> &cycle,
            span<u8> data, bool pageAlign) {
        auto allocation{Allocate(cycle, data.size(), pageAlign)};
        allocation.region.copy_from(data);
        return allocation;
    }
}
