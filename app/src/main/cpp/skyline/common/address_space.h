// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once
#include "diagnostic_recorder.h"

#include <boost/container/small_vector.hpp>
#include <concepts>
#include <common.h>
#include "segment_table.h"
#include "spin_lock.h"

namespace skyline {
    template<typename VaType, size_t AddressSpaceBits>
    concept AddressSpaceValid = std::is_unsigned_v<VaType> && sizeof(VaType) * 8 >= AddressSpaceBits;

    using TranslatedAddressRange = boost::container::small_vector<span<u8>, 1>;

    struct EmptyStruct {};

    /**
     * @brief FlatAddressSpaceMap provides a generic VA->PA mapping implementation using a sorted vector
     */
    template<typename VaType, VaType UnmappedVa, typename PaType, PaType UnmappedPa, bool PaContigSplit, size_t AddressSpaceBits, typename ExtraBlockInfo = EmptyStruct> requires AddressSpaceValid<VaType, AddressSpaceBits>
    class FlatAddressSpaceMap {
      private:
        std::function<void(VaType, VaType)> unmapCallback{}; //!< Callback called when the mappings in an region have changed

      protected:
        /**
         * @brief Represents a block of memory in the AS, the physical mapping is contiguous until another block with a different phys address is hit
         */
        struct Block {
            VaType virt{UnmappedVa}; //!< VA of the block
            PaType phys{UnmappedPa}; //!< PA of the block, will increase 1-1 with VA until a new block is encountered
            [[no_unique_address]] ExtraBlockInfo extraInfo;

            Block() = default;

            Block(VaType virt, PaType phys, ExtraBlockInfo extraInfo) : virt(virt), phys(phys), extraInfo(extraInfo) {}

            constexpr bool Valid() {
                return virt != UnmappedVa;
            }

            constexpr bool Mapped() {
                return phys != UnmappedPa;
            }

            constexpr bool Unmapped() {
                return phys == UnmappedPa;
            }

            bool operator<(const VaType &pVirt) const {
                return virt < pVirt;
            }
        };

        SharedSpinLock blockMutex;
        std::vector<Block> blocks{Block{}};

        /**
         * @brief Maps a PA range into the given AS region
         * @note blockMutex MUST be locked when calling this
         */
        void MapLocked(VaType virt, PaType phys, VaType size, ExtraBlockInfo extraInfo);

        /**
         * @brief Unmaps the given range and merges it with other unmapped regions
         * @note blockMutex MUST be locked when calling this
         */
        void UnmapLocked(VaType virt, VaType size);

      public:
        static constexpr VaType VaMaximum{(1ULL << (AddressSpaceBits - 1)) + ((1ULL << (AddressSpaceBits - 1)) - 1)}; //!< The maximum VA that this AS can technically reach

        VaType vaLimit{VaMaximum}; //!< A soft limit on the maximum VA of the AS

        FlatAddressSpaceMap(VaType vaLimit, std::function<void(VaType, VaType)> unmapCallback = {});

        FlatAddressSpaceMap() = default;
    };

    /**
     * @brief Hold memory manager specific block info
     */
    struct MemoryManagerBlockInfo {
        bool sparseMapped;
    };

    /**
     * @brief FlatMemoryManager specialises FlatAddressSpaceMap to focus on pointers as PAs, adding read/write functions and sparse mapping support
     */
    template<typename VaType, VaType UnmappedVa, size_t AddressSpaceBits, size_t VaGranularityBits, size_t VaL2GranularityBits> requires AddressSpaceValid<VaType, AddressSpaceBits>
    class FlatMemoryManager : public FlatAddressSpaceMap<VaType, UnmappedVa, u8 *, nullptr, true, AddressSpaceBits, MemoryManagerBlockInfo> {
      private:
        static constexpr u64 SparseMapSize{0x400000000}; //!< 16GiB pool size for sparse mappings returned by TranslateRange, this number is arbritary and should be large enough to fit the largest sparse mapping in the AS
        u8 *sparseMap; //!< Pointer to a zero filled memory region that is returned by TranslateRange for sparse mappings

        /**
         * @brief Version of `Block` that is trivial so it can be stored in a segment table for rapid lookups, also holds an additional extent member
         */
        struct SegmentTableEntry {
            VaType virt;
            u8 *phys;
            VaType extent;
            MemoryManagerBlockInfo extraInfo;
        };

        static constexpr size_t AddressSpaceSize{1ULL << AddressSpaceBits};
        SegmentTable<SegmentTableEntry, AddressSpaceSize, VaGranularityBits, VaL2GranularityBits> blockSegmentTable; //!< A page table of all buffer mappings for O(1) lookups on full matches
        bool hasUnalignedMappings{}; //!< Sub-page boundaries require exact lookups in the block vector

        void ValidateRange(VaType virt, VaType size) const {
            if (virt < UnmappedVa || virt > this->vaLimit || size > this->vaLimit - virt)
                throw exception("GPU VA range outside address space: address 0x{:X}, size 0x{:X}", virt, size);
        }

        // Refresh both sides of a split as well as the changed range. Retaining
        // an old extent would let a fast lookup cross a later remap or hole.
        void RefreshBlockSegments(VaType virt, VaType size) {
            if (hasUnalignedMappings)
                return;

            blockSegmentTable.Set(virt, virt + size, {});
            auto block{std::lower_bound(this->blocks.begin(), this->blocks.end(), virt)};
            if (block != this->blocks.begin())
                --block;
            for (; block != this->blocks.end() && block->virt <= virt + size; ++block) {
                if (!block->phys)
                    continue;
                auto end{std::next(block)->virt}; // Mapped blocks always have an unmapped tail
                blockSegmentTable.Set(block->virt, end, {block->virt, block->phys, end - block->virt, block->extraInfo});
            }
        }

        TranslatedAddressRange TranslateRangeImpl(VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {});

        std::pair<span<u8>, size_t> LookupBlockLocked(VaType virt, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            ValidateRange(virt, 1);
            if (hasUnalignedMappings) {
                auto successor{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt, [](auto address, const auto &block) {
                    return address < block.virt;
                })};
                const auto &block{*std::prev(successor)};
                auto end{successor == this->blocks.end() ? this->vaLimit : successor->virt};
                span<u8> mapping{block.extraInfo.sparseMapped ? nullptr : block.phys, end - block.virt};
                if (mapping.valid() && cpuAccessCallback)
                    cpuAccessCallback(mapping);
                return {mapping, virt - block.virt};
            }
            const auto &blockEntry{this->blockSegmentTable[virt]};
            VaType segmentOffset{virt - blockEntry.virt};

            if (blockEntry.extraInfo.sparseMapped || blockEntry.phys == nullptr)
                return {span<u8>{static_cast<u8*>(nullptr), blockEntry.extent}, segmentOffset};

            span<u8> blockSpan{blockEntry.phys, blockEntry.extent};
            if (cpuAccessCallback)
                cpuAccessCallback(blockSpan);

            return {blockSpan, segmentOffset};
        }

      public:
        const diagnostics::Id diagnosticSpaceId{diagnostics::NewId()};
        FlatMemoryManager();

        ~FlatMemoryManager();

        /**
         * @return A placeholder address for sparse mapped regions, this means nothing
         */
        static u8 *SparsePlaceholderAddress() {
            return reinterpret_cast<u8 *>(0xCAFEBABE);
        }

        /**
         * @brief Looks up the mapped region that contains the given VA
         * @return A span of the mapped region and the offset of the input VA in the region
         */
        __attribute__((always_inline)) std::pair<span<u8>, VaType> LookupBlock(VaType virt, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            std::shared_lock lock{this->blockMutex};
            return LookupBlockLocked(virt, cpuAccessCallback);
        }

        /**
         * @brief Translates a region in the VA space to a corresponding set of regions in the PA space
         */
        TranslatedAddressRange TranslateRange(VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            std::shared_lock lock{this->blockMutex};
            ValidateRange(virt, size);
            if (!size)
                return {};

            // Fast path for when the range is mapped in a single block
            auto [blockSpan, rangeOffset]{LookupBlockLocked(virt, cpuAccessCallback)};
            if (rangeOffset <= blockSpan.size() && size <= blockSpan.size() - rangeOffset) {
                TranslatedAddressRange ranges;
                ranges.push_back({blockSpan.valid() ? blockSpan.data() + rangeOffset : nullptr, size});
                return ranges;
            }

            return TranslateRangeImpl(virt, size, cpuAccessCallback);
        }


        void Read(u8 *destination, VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {});

        template<typename T>
        void Read(span <T> destination, VaType virt, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            Read(reinterpret_cast<u8 *>(destination.data()), virt, destination.size_bytes(), cpuAccessCallback);
        }

        template<typename T>
        T Read(VaType virt, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            T obj;
            Read(reinterpret_cast<u8 *>(&obj), virt, sizeof(T), cpuAccessCallback);
            return obj;
        }

        /**
         * @brief Writes contents starting from the virtual address till the end of the span or an unmapped block has been hit or when `function` returns a non-nullopt value
         * @param function A function that is called on every block where it should return an end offset into the block when it wants to end reading or std::nullopt when it wants to continue reading
         * @return A span into the supplied container with the contents of the memory region
         * @note The function will **NOT** be run on any sparse block
         * @note The function will provide no feedback on if the end has been reached or if there was an early exit
         */
        template<typename Function, typename Container>
        span<u8> ReadTill(Container& destination, VaType virt, Function function, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            std::shared_lock lock(this->blockMutex);
            ValidateRange(virt, 0);
            // The exclusive VA limit is an unmapped boundary for this partial reader.
            const VaType total{static_cast<VaType>(std::min<size_t>(destination.size(), this->vaLimit - virt))};
            VaType remaining{total};
            auto pointer{destination.data()};
            while (remaining) {
                auto next{std::upper_bound(this->blocks.begin(), this->blocks.end(), virt,
                    [](VaType value, const auto &block) { return value < block.virt; })};
                auto block{std::prev(next)};
                if (!block->phys)
                    break; // ReadTill deliberately returns the valid prefix.
                const VaType end{next == this->blocks.end() ? this->vaLimit : next->virt};
                const VaType amount{std::min(remaining, end - virt)};
                if (block->extraInfo.sparseMapped) {
                    std::memset(pointer, 0, amount);
                } else {
                    span<u8> cpuBlock{block->phys + (virt - block->virt), amount};
                    if (cpuAccessCallback)
                        cpuAccessCallback(cpuBlock);
                    auto stop{function(cpuBlock)};
                    if (stop && *stop > amount)
                        throw exception("ReadTill callback exceeded its block");
                    const auto count{stop ? *stop : amount};
                    std::memcpy(pointer, cpuBlock.data(), count);
                    if (stop)
                        return {destination.data(), total - remaining + count};
                }
                pointer += amount;
                virt += amount;
                remaining -= amount;
            }
            return {destination.data(), total - remaining};
        }

        void Write(VaType virt, u8 *source, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {});

        template<typename T>
        void Write(VaType virt, span<T> source, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            Write(virt, reinterpret_cast<u8 *>(source.data()), source.size_bytes(), cpuAccessCallback);
        }

        void Write(VaType virt, util::TrivialObject auto source, std::function<void(span<u8>)> cpuAccessCallback = {}) {
            Write(virt, reinterpret_cast<u8 *>(&source), sizeof(source), cpuAccessCallback);
        }

        void Copy(VaType dst, VaType src, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {});

        void Map(VaType virt, u8 *phys, VaType size, MemoryManagerBlockInfo extraInfo = {}) {
            std::scoped_lock lock(this->blockMutex);
            ValidateRange(virt, size);
            if (!size)
                return;
            this->MapLocked(virt, phys, size, extraInfo);
            diagnostics::Mapping(diagnosticSpaceId, virt, size, reinterpret_cast<uintptr_t>(phys), extraInfo.sparseMapped, false);
            hasUnalignedMappings |= ((virt | size) & ((1ULL << VaGranularityBits) - 1)) != 0;
            RefreshBlockSegments(virt, size);
        }

        void Unmap(VaType virt, VaType size) {
            std::scoped_lock lock(this->blockMutex);
            ValidateRange(virt, size);
            if (!size)
                return;
            this->UnmapLocked(virt, size);
            diagnostics::Mapping(diagnosticSpaceId, virt, size, 0, false, true);
            hasUnalignedMappings |= ((virt | size) & ((1ULL << VaGranularityBits) - 1)) != 0;
            RefreshBlockSegments(virt, size);
        }
    };

    /**
     * @brief FlatMemoryManager specialises FlatAddressSpaceMap to work as an allocator, with an initial, fast linear pass and a subsequent slower pass that iterates until it finds a free block
     */
    template<typename VaType, VaType UnmappedVa, size_t AddressSpaceBits> requires AddressSpaceValid<VaType, AddressSpaceBits>
    class FlatAllocator : public FlatAddressSpaceMap<VaType, UnmappedVa, bool, false, false, AddressSpaceBits> {
      private:
        using Base = FlatAddressSpaceMap<VaType, UnmappedVa, bool, false, false, AddressSpaceBits>;

        VaType currentLinearAllocEnd; //!< The end address for the initial linear allocation pass, once this reaches the AS limit the slower allocation path will be used

      public:
        VaType vaStart; //!< The base VA of the allocator, no allocations will be below this

        FlatAllocator(VaType vaStart, VaType vaLimit = Base::VaMaximum);

        /**
         * @brief Allocates a region in the AS of the given size and returns its address
         */
        VaType Allocate(VaType size);

        /**
         * @brief Marks the given region in the AS as allocated
         */
        void AllocateFixed(VaType virt, VaType size);

        /**
         * @brief Frees an AS region so it can be used again
         */
        void Free(VaType virt, VaType size);
    };
}

