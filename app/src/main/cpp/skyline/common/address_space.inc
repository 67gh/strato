// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <kernel/types/KProcess.h>
#include "address_space.h"

#define MAP_MEMBER(returnType) template<typename VaType, VaType UnmappedVa, typename PaType, PaType UnmappedPa, bool PaContigSplit, size_t AddressSpaceBits, typename ExtraBlockInfo> requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatAddressSpaceMap<VaType, UnmappedVa, PaType, UnmappedPa, PaContigSplit, AddressSpaceBits, ExtraBlockInfo>

#define MM_MEMBER(returnType) template<typename VaType, VaType UnmappedVa, size_t AddressSpaceBits, size_t VaGranularityBits, size_t VaL2GranularityBits> requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatMemoryManager<VaType, UnmappedVa, AddressSpaceBits, VaGranularityBits, VaL2GranularityBits>

#define ALLOC_MEMBER(returnType) template<typename VaType, VaType UnmappedVa, size_t AddressSpaceBits> requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatAllocator<VaType, UnmappedVa, AddressSpaceBits>

namespace skyline {
    MAP_MEMBER()::FlatAddressSpaceMap(VaType vaLimit, std::function<void(VaType, VaType)> unmapCallback) :
        vaLimit(vaLimit),
        unmapCallback(std::move(unmapCallback)) {
        if (vaLimit > VaMaximum)
            throw exception("Invalid VA limit!");
    }

    MAP_MEMBER(void)::MapLocked(VaType virt, PaType phys, VaType size, ExtraBlockInfo extraInfo) {
        TRACE_EVENT("containers", "FlatAddressSpaceMap::Map");
        if (virt < UnmappedVa || virt > vaLimit || size > vaLimit - virt)
            throw exception("Trying to map outside VA limits: address 0x{:X}, size 0x{:X}", virt, size);
        if (!size)
            return;

        const VaType virtEnd{virt + size};
        auto endSuccessor{std::upper_bound(blocks.begin(), blocks.end(), virtEnd, [](auto address, const auto &block) {
            return address < block.virt;
        })};
        const auto &oldTail{*std::prev(endSuccessor)};
        PaType tailPhys{oldTail.phys};
        if constexpr (PaContigSplit) {
            if (oldTail.phys != UnmappedPa)
                tailPhys += virtEnd - oldTail.virt;
        }
        Block tail{virtEnd, tailPhys, oldTail.extraInfo};
        auto start{std::lower_bound(blocks.begin(), blocks.end(), virt)};
        auto position{blocks.erase(start, endSuccessor)};
        blocks.insert(position, {Block{virt, phys, extraInfo}, tail});

        // Keep one boundary for each hole, including the terminal hole. This
        // also handles replacements at the very first address without prev(begin).
        blocks.erase(std::unique(blocks.begin(), blocks.end(), [](auto &left, auto &right) {
            return left.Unmapped() && right.Unmapped();
        }), blocks.end());
        if (unmapCallback)
            unmapCallback(virt, size);
    }

    MAP_MEMBER(void)::UnmapLocked(VaType virt, VaType size) {
        MapLocked(virt, UnmappedPa, size, {});
    }

    MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl(VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TRACE_EVENT("containers", "FlatMemoryManager::TranslateRange");
        TranslatedAddressRange ranges;
        auto successor{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt, [](auto address, const auto &block) {
            return address < block.virt;
        })};
        auto block{std::prev(successor)};
        while (size) {
            const VaType end{successor == this->blocks.end() ? this->vaLimit : successor->virt};
            const VaType amount{std::min(end - virt, size)};
            // Sparse placeholders are not host pointers that callers may access.
            u8 *phys{block->phys && !block->extraInfo.sparseMapped ? block->phys + (virt - block->virt) : nullptr};
            span<u8> range{phys, amount};
            if (phys && cpuAccessCallback)
                cpuAccessCallback(range);
            if (!ranges.empty() && ((phys && ranges.back().valid() && ranges.back().data() + ranges.back().size() == phys) ||
                                    (!phys && !ranges.back().valid())))
                ranges.back() = {ranges.back().data(), ranges.back().size() + amount};
            else
                ranges.push_back(range);
            size -= amount;
            virt += amount;
            if (size)
                block = successor++;
        }
        return ranges;
    }

    MM_MEMBER()::FlatMemoryManager() {
        // Both vaLimit and the terminal block must represent the exclusive end.
        // In particular, a 32-bit address space needs wider internal addresses.
        static_assert(AddressSpaceBits < sizeof(VaType) * 8,
                      "FlatMemoryManager needs a VA type wide enough for its exclusive end");
        this->vaLimit = AddressSpaceSize; // Exclusive end, including the final GPU page
        sparseMap = static_cast<u8 *>(mmap(0, SparseMapSize, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
        if (sparseMap == MAP_FAILED)
            throw exception("Failed to mmap sparse map!");
    }

    MM_MEMBER()::~FlatMemoryManager() {
        diagnostics::DestroyAddressSpace(diagnosticSpaceId);
        munmap(sparseMap, SparseMapSize);
    }

    MM_MEMBER(void)::Read(u8 *destination, VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TRACE_EVENT("containers", "FlatMemoryManager::Read");
        std::shared_lock lock(this->blockMutex);
        ValidateRange(virt, size);
        if (!size)
            return;
        auto next{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt,
            [](VaType value, const auto &block) { return value < block.virt; })};
        auto block{std::prev(next)};
        while (size) {
            if (!block->phys)
                throw exception("Page fault at 0x{:X}", virt);
            const VaType end{next == this->blocks.end() ? this->vaLimit : next->virt};
            const VaType amount{std::min(size, end - virt)};
            if (block->extraInfo.sparseMapped) {
                std::memset(destination, 0, amount);
            } else {
                u8 *physical{block->phys + (virt - block->virt)};
                if (cpuAccessCallback)
                    cpuAccessCallback(span{physical, amount});
                std::memcpy(destination, physical, amount);
            }
            destination += amount;
            virt += amount;
            size -= amount;
            if (size)
                block = next++;
        }
    }

    MM_MEMBER(void)::Write(VaType virt, u8 *source, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TRACE_EVENT("containers", "FlatMemoryManager::Write");
        std::shared_lock lock(this->blockMutex);
        ValidateRange(virt, size);
        if (!size)
            return;
        auto next{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt,
            [](VaType value, const auto &block) { return value < block.virt; })};
        auto block{std::prev(next)};
        while (size) {
            if (!block->phys)
                throw exception("Page fault at 0x{:X}", virt);
            const VaType end{next == this->blocks.end() ? this->vaLimit : next->virt};
            const VaType amount{std::min(size, end - virt)};
            if (!block->extraInfo.sparseMapped) {
                u8 *physical{block->phys + (virt - block->virt)};
                if (cpuAccessCallback)
                    cpuAccessCallback(span{physical, amount});
                std::memcpy(physical, source, amount);
            }
            source += amount;
            virt += amount;
            size -= amount;
            if (size)
                block = next++;
        }
    }

    MM_MEMBER(void)::Copy(VaType dst, VaType src, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TRACE_EVENT("containers", "FlatMemoryManager::Copy");
        std::shared_lock lock(this->blockMutex);
        ValidateRange(src, size);
        ValidateRange(dst, size);
        const bool backward{dst > src && dst - src < size};
        VaType remaining{size};
        while (remaining) {
            const VaType srcAddress{backward ? src + remaining - 1 : src + size - remaining};
            const VaType dstAddress{backward ? dst + remaining - 1 : dst + size - remaining};
            auto srcNext{std::upper_bound(this->blocks.begin(), this->blocks.end(), srcAddress,
                [](VaType value, const auto &block) { return value < block.virt; })};
            auto dstNext{std::upper_bound(this->blocks.begin(), this->blocks.end(), dstAddress,
                [](VaType value, const auto &block) { return value < block.virt; })};
            auto source{std::prev(srcNext)};
            auto destination{std::prev(dstNext)};
            if (!source->phys || !destination->phys)
                throw exception("Page fault copying GPU VA 0x{:X} to 0x{:X}", srcAddress, dstAddress);
            const VaType srcAvailable{backward ? srcAddress - source->virt + 1 :
                (srcNext == this->blocks.end() ? this->vaLimit : srcNext->virt) - srcAddress};
            const VaType dstAvailable{backward ? dstAddress - destination->virt + 1 :
                (dstNext == this->blocks.end() ? this->vaLimit : dstNext->virt) - dstAddress};
            const VaType amount{std::min({remaining, srcAvailable, dstAvailable})};
            // Sparse destinations discard all writes, including zero-fill from sparse sources.
            if (!destination->extraInfo.sparseMapped) {
                u8 *dstPhysical{destination->phys + (dstAddress - destination->virt) - (backward ? amount - 1 : 0)};
                if (cpuAccessCallback)
                    cpuAccessCallback(span{dstPhysical, amount});
                if (source->extraInfo.sparseMapped) {
                    std::memset(dstPhysical, 0, amount);
                } else {
                    u8 *srcPhysical{source->phys + (srcAddress - source->virt) - (backward ? amount - 1 : 0)};
                    if (cpuAccessCallback)
                        cpuAccessCallback(span{srcPhysical, amount});
                    std::memmove(dstPhysical, srcPhysical, amount);
                }
            }
            remaining -= amount;
        }
    }

    ALLOC_MEMBER()::FlatAllocator(VaType vaStart, VaType vaLimit) : Base(vaLimit), vaStart(vaStart), currentLinearAllocEnd(vaStart) {
        if (vaStart <= UnmappedVa || vaStart >= vaLimit)
            throw exception("Invalid allocator bounds");
    }

    ALLOC_MEMBER(VaType)::Allocate(VaType size) {
        TRACE_EVENT("containers", "FlatAllocator::Allocate");
        std::scoped_lock lock(this->blockMutex);
        if (!size || vaStart >= this->vaLimit || size > this->vaLimit - vaStart)
            return UnmappedVa;

        auto findGap = [&](VaType start) -> VaType {
            if (start >= this->vaLimit)
                return UnmappedVa;
            auto next{std::upper_bound(this->blocks.begin(), this->blocks.end(), start,
                [](VaType value, const auto &block) { return value < block.virt; })};
            if (next == this->blocks.begin())
                return UnmappedVa;
            auto block{std::prev(next)};
            while (block != this->blocks.end()) {
                const VaType end{next == this->blocks.end() ? this->vaLimit : next->virt};
                const VaType begin{std::max(start, block->virt)};
                if (!block->Mapped() && begin < end && size <= end - begin)
                    return begin;
                block = next;
                if (next != this->blocks.end())
                    ++next;
            }
            return UnmappedVa;
        };

        VaType address{findGap(std::max(vaStart, currentLinearAllocEnd))};
        if (address == UnmappedVa)
            address = findGap(vaStart);
        if (address == UnmappedVa)
            return UnmappedVa;
        this->MapLocked(address, true, size, {});
        currentLinearAllocEnd = address + size;
        return address;
    }

    ALLOC_MEMBER(void)::AllocateFixed(VaType virt, VaType size) {
        std::scoped_lock lock(this->blockMutex);
        if (!size || virt < vaStart || virt > this->vaLimit || size > this->vaLimit - virt)
            throw exception("Invalid fixed allocation range");
        auto next{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt,
            [](VaType value, const auto &block) { return value < block.virt; })};
        if (next == this->blocks.begin())
            throw exception("Invalid allocator map");
        auto block{std::prev(next)};
        if (block->Mapped() || (next != this->blocks.end() && size > next->virt - virt))
            throw exception("Fixed allocation overlaps an existing allocation");
        this->MapLocked(virt, true, size, {});
    }

    ALLOC_MEMBER(void)::Free(VaType virt, VaType size) {
        std::scoped_lock lock(this->blockMutex);
        this->UnmapLocked(virt, size);
    }
}

