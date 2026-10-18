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

        auto successor{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt, [] (auto virt, const auto &block) {
            return virt < block.virt;
        })};

        auto predecessor{std::prev(successor)};

        u8 *blockPhys{predecessor->phys + (virt - predecessor->virt)};
        VaType blockReadSize{std::min(successor->virt - virt, size)};

        // Reads may span across multiple individual blocks
        while (size) {
            if (predecessor->phys == nullptr) {
                throw exception("Page fault at 0x{:X}", predecessor->virt);
            } else {
                if (predecessor->extraInfo.sparseMapped) { // Sparse mappings read all zeroes
                    std::memset(destination, 0, blockReadSize);
                } else {
                    if (cpuAccessCallback)
                        cpuAccessCallback(span{blockPhys, blockReadSize});

                    std::memcpy(destination, blockPhys, blockReadSize);
                }
            }

            destination += blockReadSize;
            size -= blockReadSize;

            if (size) {
                predecessor = successor++;
                blockPhys = predecessor->phys;
                blockReadSize = std::min(successor->virt - predecessor->virt, size);
            }
        }
    }

    MM_MEMBER(void)::Write(VaType virt, u8 *source, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TRACE_EVENT("containers", "FlatMemoryManager::Write");

        std::shared_lock lock(this->blockMutex);

        VaType virtEnd{virt + size};

        auto successor{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt, [] (auto virt, const auto &block) {
            return virt < block.virt;
        })};

        auto predecessor{std::prev(successor)};

        u8 *blockPhys{predecessor->phys + (virt - predecessor->virt)};
        VaType blockWriteSize{std::min(successor->virt - virt, size)};

        // Writes may span across multiple individual blocks
        while (size) {
            if (predecessor->phys == nullptr) {
                throw exception("Page fault at 0x{:X}", predecessor->virt);
            } else {
                if (!predecessor->extraInfo.sparseMapped) { // Sparse mappings ignore writes
                    if (cpuAccessCallback)
                        cpuAccessCallback(span{blockPhys, blockWriteSize});

                    std::memcpy(blockPhys, source, blockWriteSize);
                }
            }

            source += blockWriteSize;
            size -= blockWriteSize;

            if (size) {
                predecessor = successor++;
                blockPhys = predecessor->phys;
                blockWriteSize = std::min(successor->virt - predecessor->virt, size);
            }
        }
    }

    MM_MEMBER(void)::Copy(VaType dst, VaType src, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TRACE_EVENT("containers", "FlatMemoryManager::Copy");

        std::shared_lock lock(this->blockMutex);

        VaType srcEnd{src + size};
        VaType dstEnd{dst + size};

        auto srcSuccessor{std::upper_bound(this->blocks.begin(), this->blocks.end(), src, [] (auto virt, const auto &block) {
            return virt < block.virt;
        })};

        auto dstSuccessor{std::upper_bound(this->blocks.begin(), this->blocks.end(), dst, [] (auto virt, const auto &block) {
            return virt < block.virt;
        })};

        auto srcPredecessor{std::prev(srcSuccessor)};
        auto dstPredecessor{std::prev(dstSuccessor)};

        u8 *srcBlockPhys{srcPredecessor->phys + (src - srcPredecessor->virt)};
        u8 *dstBlockPhys{dstPredecessor->phys + (dst - dstPredecessor->virt)};

        VaType srcBlockRemainingSize{srcSuccessor->virt - src};
        VaType dstBlockRemainingSize{dstSuccessor->virt - dst};

        VaType blockCopySize{std::min({srcBlockRemainingSize, dstBlockRemainingSize, size})};

        // Writes may span across multiple individual blocks
        while (size) {
            if (srcPredecessor->phys == nullptr) {
                throw exception("Page fault at 0x{:X}", srcPredecessor->virt);
            } else if (dstPredecessor->phys == nullptr) {
                throw exception("Page fault at 0x{:X}", dstPredecessor->virt);
            } else { [[likely]]
                    if (srcPredecessor->extraInfo.sparseMapped) {
                        std::memset(dstBlockPhys, 0, blockCopySize);
                    } else [[likely]] {
                        if (cpuAccessCallback) {
                            cpuAccessCallback(span{dstBlockPhys, blockCopySize});
                            cpuAccessCallback(span{srcBlockPhys, blockCopySize});
                        }

                        std::memcpy(dstBlockPhys, srcBlockPhys, blockCopySize);
                    }
            }

            dstBlockPhys += blockCopySize;
            srcBlockPhys += blockCopySize;
            size -= blockCopySize;
            srcBlockRemainingSize -= blockCopySize;
            dstBlockRemainingSize -= blockCopySize;

            if (size) {
                if (!srcBlockRemainingSize) {
                    srcPredecessor = srcSuccessor++;
                    srcBlockPhys = srcPredecessor->phys;
                    srcBlockRemainingSize = srcSuccessor->virt - srcPredecessor->virt;
                    blockCopySize = std::min({srcBlockRemainingSize, dstBlockRemainingSize, size});
                }
                if (!dstBlockRemainingSize) {
                    dstPredecessor = dstSuccessor++;
                    dstBlockPhys = dstPredecessor->phys;
                    dstBlockRemainingSize = dstSuccessor->virt - dstPredecessor->virt;
                    blockCopySize = std::min({srcBlockRemainingSize, dstBlockRemainingSize, size});
                }
            }
        }
    }

    ALLOC_MEMBER()::FlatAllocator(VaType vaStart, VaType vaLimit) : Base(vaLimit), vaStart(vaStart), currentLinearAllocEnd(vaStart) {}

    ALLOC_MEMBER(VaType)::Allocate(VaType size) {
        TRACE_EVENT("containers", "FlatAllocator::Allocate");

        std::scoped_lock lock(this->blockMutex);

        VaType allocStart{UnmappedVa};
        VaType allocEnd{currentLinearAllocEnd + size};

        // Avoid searching backwards in the address space if possible
        if (allocEnd >= currentLinearAllocEnd && allocEnd <= this->vaLimit) {
            auto allocEndSuccessor{std::lower_bound(this->blocks.begin(), this->blocks.end(), allocEnd)};
            if (allocEndSuccessor == this->blocks.begin())
                throw exception("First block in AS map is invalid!");

            auto allocEndPredecessor{std::prev(allocEndSuccessor)};
            if (allocEndPredecessor->virt <= currentLinearAllocEnd) {
                allocStart = currentLinearAllocEnd;
            } else {
                // Skip over fixed any mappings in front of us
                while (allocEndSuccessor != this->blocks.end()) {
                    if (allocEndSuccessor->virt - allocEndPredecessor->virt < size || allocEndPredecessor->Mapped() ) {
                        allocStart = allocEndPredecessor->virt;
                        break;
                    }

                    allocEndPredecessor = allocEndSuccessor++;

                    // Use the VA limit to calculate if we can fit in the final block since it has no successor
                    if (allocEndSuccessor == this->blocks.end()) {
                        allocEnd = allocEndPredecessor->virt + size;

                        if (allocEnd >= allocEndPredecessor->virt && allocEnd <= this->vaLimit)
                            allocStart = allocEndPredecessor->virt;
                    }
                }
            }
        }

        if (allocStart != UnmappedVa) {
            currentLinearAllocEnd = allocStart + size;
        } else {  // If linear allocation overflows the AS then find a gap
            if (this->blocks.size() <= 2)
                throw exception("Unexpected allocator state!");

            auto searchPredecessor{std::next(this->blocks.begin())};
            auto searchSuccessor{std::next(searchPredecessor)};

            while (searchSuccessor != this->blocks.end() &&
                (searchSuccessor->virt - searchPredecessor->virt < size || searchPredecessor->Mapped())) {
                searchPredecessor = searchSuccessor++;
            }

            if (searchSuccessor != this->blocks.end())
                allocStart = searchPredecessor->virt;
            else
                return {}; // AS is full
        }


        this->MapLocked(allocStart, true, size, {});
        return allocStart;
    }

    ALLOC_MEMBER(void)::AllocateFixed(VaType virt, VaType size) {
        std::scoped_lock lock(this->blockMutex);
        this->MapLocked(virt, true, size, {});
    }

    ALLOC_MEMBER(void)::Free(VaType virt, VaType size) {
        std::scoped_lock lock(this->blockMutex);
        this->UnmapLocked(virt, size);
    }
}
