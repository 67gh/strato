// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <fstream>
#include <charconv>
#include <cstdint>
#include <limits>
#include <asm-generic/unistd.h>
#include <fcntl.h>
#include "memory.h"
#include "types/KProcess.h"

namespace skyline::kernel {
    MemoryManager::MemoryManager(const DeviceState &state) noexcept : state{state}, processHeapSize{}, memRefs{} {}

    MemoryManager::~MemoryManager() noexcept {
        if (base.valid() && !base.empty())
            munmap(reinterpret_cast<void *>(base.data()), base.size());
        if (codeBase36Bit.valid() && !codeBase36Bit.empty())
            munmap(reinterpret_cast<void *>(codeBase36Bit.data()), codeBase36Bit.size());
    }

    void MemoryManager::MapInternal(const std::pair<u8 *, ChunkDescriptor> &newDesc, bool updateHostProtection) {
        if (!newDesc.second.size)
            return;
        const auto startAddress{reinterpret_cast<uintptr_t>(newDesc.first)};
        if (newDesc.second.size > std::numeric_limits<uintptr_t>::max() - startAddress || chunks.empty()) [[unlikely]]
            throw exception("Invalid memory mapping range");
        u8 *end{newDesc.first + newDesc.second.size};
        auto current{chunks.upper_bound(newDesc.first)};
        if (current == chunks.begin() || end > chunks.rbegin()->first) [[unlikely]]
            throw exception("Memory mapping range is outside the descriptor map");
        --current;
        const bool isUnmapping{newDesc.second.state == memory::states::Unmapped};
        bool needsReprotection{};
        for (; current != chunks.end() && current->first < end; ++current) {
            if (current->second.size && (current->second.state == memory::states::Unmapped) != isUnmapping) {
                needsReprotection = true;
                break;
            }
        }
        // Do not publish a mapped/unmapped state if the host protection fails.
        // Mapped-to-mapped guest permission edits must preserve GPU trap protections.
        if (needsReprotection && updateHostProtection) {
            auto hostSpan{GetHostSpan({newDesc.first, newDesc.second.size})};
            if (mprotect(hostSpan.data(), hostSpan.size(), isUnmapping ? PROT_NONE : PROT_READ | PROT_WRITE | PROT_EXEC)) [[unlikely]] {
                int error{errno};
                LOGE("VMM reprotection failed at {} (0x{:X} bytes): {}", fmt::ptr(hostSpan.data()), hostSpan.size(), strerror(error));
                throw exception("VMM reprotection failed: {}", strerror(error));
            }
        }

        // The chunk that contains / precedes the new chunk base address
        auto firstChunkBase{chunks.lower_bound(newDesc.first)};
        if (newDesc.first <= firstChunkBase->first && firstChunkBase != chunks.begin())
            --firstChunkBase;

        // The chunk that contains / follows the end address of the new chunk
        auto lastChunkBase{chunks.lower_bound(newDesc.first + newDesc.second.size)};
        if ((newDesc.first + newDesc.second.size) < lastChunkBase->first)
            --lastChunkBase;

        ChunkDescriptor firstChunk{firstChunkBase->second};
        ChunkDescriptor lastChunk{lastChunkBase->second};

        // We cut a hole in a single chunk
        if (firstChunkBase->first == lastChunkBase->first) {
            if (firstChunk.IsCompatible(newDesc.second)) [[unlikely]]
                // No editing necessary
                return;

            // We reduce the size of the first half
            firstChunk.size = static_cast<size_t>(newDesc.first - firstChunkBase->first);
            chunks.insert_or_assign(firstChunkBase->first, firstChunk);

            // We create the chunk's second half
            lastChunk.size = static_cast<size_t>((lastChunkBase->first + lastChunk.size) - (newDesc.first + newDesc.second.size));
            chunks.insert_or_assign(newDesc.first + newDesc.second.size, lastChunk);

            // Insert new chunk in between
            chunks.insert_or_assign(newDesc.first, newDesc.second);
        } else {
            // If there are descriptors between first and last chunk, delete them
            if ((firstChunkBase->first + firstChunk.size) != lastChunkBase->first) {
                chunks.erase(std::next(firstChunkBase), lastChunkBase);
            }

            bool shouldInsert{true};

            // We check if the new chunk and the first chunk is mergeable
            if (firstChunk.IsCompatible(newDesc.second)) {
                shouldInsert = false;

                firstChunk.size = static_cast<size_t>((newDesc.first + newDesc.second.size) - firstChunkBase->first);
                chunks.insert_or_assign(firstChunkBase->first, firstChunk);
            } else if ((firstChunkBase->first + firstChunk.size) != newDesc.first) { // If it's not mergeable check if it needs resizing
                firstChunk.size = static_cast<size_t>(newDesc.first - firstChunkBase->first);

                chunks.insert_or_assign(firstChunkBase->first, firstChunk);
            }

            // We check if the new chunk and the last chunk is mergeable
            if (lastChunk.IsCompatible(newDesc.second)) {
                u8 *oldBase{lastChunkBase->first};
                chunks.erase(lastChunkBase);

                if (shouldInsert) {
                    shouldInsert = false;

                    lastChunk.size = static_cast<size_t>((lastChunk.size + oldBase) - (newDesc.first));

                    chunks.insert_or_assign(newDesc.first, lastChunk);
                } else {
                    firstChunk.size = static_cast<size_t>((lastChunk.size + oldBase) - firstChunkBase->first);
                    chunks.insert_or_assign(firstChunkBase->first, firstChunk);
                }
            } else if ((newDesc.first + newDesc.second.size) != lastChunkBase->first) { // If it's not mergeable check if it needs resizing
                lastChunk.size = static_cast<size_t>((lastChunk.size + lastChunkBase->first) - (newDesc.first + newDesc.second.size));

                chunks.erase(lastChunkBase);
                chunks.insert_or_assign(newDesc.first + newDesc.second.size, lastChunk);
            }

            // Insert if not merged
            if (shouldInsert)
                chunks.insert_or_assign(newDesc.first, newDesc.second);
        }
    }

    void MemoryManager::ForeachChunkInRange(span<u8> memory, auto editCallback) {
        u8 *cursor{memory.data()};
        size_t sizeLeft{memory.size()};
        while (sizeLeft) {
            // MapInternal may erase or merge both the current and following nodes.
            // Keep only the clipped descriptor across the callback, then look up again.
            auto chunkBase{chunks.upper_bound(cursor)};
            if (chunkBase == chunks.begin()) [[unlikely]]
                throw exception("Memory range has no chunk descriptor");
            --chunkBase;
            size_t offset{static_cast<size_t>(cursor - chunkBase->first)};
            if (offset >= chunkBase->second.size) [[unlikely]]
                throw exception("Memory range extends beyond its chunk descriptors");

            size_t chunkSize{std::min(sizeLeft, chunkBase->second.size - offset)};
            std::pair<u8 *, ChunkDescriptor> temp{cursor, chunkBase->second};
            temp.second.size = chunkSize;
            editCallback(temp);
            cursor += chunkSize;
            sizeLeft -= chunkSize;
        }
    }

    static std::vector<std::pair<uintptr_t, size_t>> FindMappingCandidates(std::istream &maps, size_t minSize, size_t align, uintptr_t minAddress, uintptr_t maxAddress, bool findLargest) {
        if (!minSize || !util::IsPageAligned(minSize) || !align || (align & (align - 1)) ||
            !util::IsPageAligned(align) || minAddress >= maxAddress || minSize > maxAddress - minAddress) [[unlikely]]
            throw exception("Invalid guest address reservation parameters");

        std::vector<std::pair<uintptr_t, size_t>> candidates;
        auto addGap = [&](uintptr_t start, uintptr_t end) {
            start = std::max(start, minAddress);
            end = std::min(end, maxAddress);
            if (start >= end || start > std::numeric_limits<uintptr_t>::max() - (align - 1))
                return;
            start = (start + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
            if (start >= end || minSize > end - start)
                return;
            size_t size{findLargest ? (end - start) & ~(constant::PageSize - 1) : minSize};
            candidates.emplace_back(start, size);
        };

        uintptr_t previousEnd{};
        bool sawMapping{};
        std::string line;
        while (std::getline(maps, line)) {
            const auto dash{line.find('-')};
            const auto space{line.find(' ')};
            if (dash == std::string::npos || space == std::string::npos || !dash || dash + 1 >= space) [[unlikely]]
                throw exception("Malformed address range in /proc/self/maps");
            uintptr_t start{}, end{};
            auto first{std::from_chars(line.data(), line.data() + dash, start, 16)};
            auto last{std::from_chars(line.data() + dash + 1, line.data() + space, end, 16)};
            if (first.ec != std::errc{} || last.ec != std::errc{} || first.ptr != line.data() + dash ||
                last.ptr != line.data() + space || start >= end || start < previousEnd) [[unlikely]]
                throw exception("Invalid or unordered address range in /proc/self/maps");
            addGap(previousEnd, start);
            previousEnd = end;
            sawMapping = true;
        }
        if (maps.bad() || !maps.eof() || !sawMapping) [[unlikely]]
            throw exception("Failed to read /proc/self/maps");
        addGap(previousEnd, maxAddress);
        if (findLargest)
            std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        return candidates;
    }

    static span<u8> AllocateMappedRange(size_t minSize, size_t align, size_t minAddress, size_t maxAddress, bool findLargest) {
        int lastError{};
        // /proc is only a placement hint. Never replace an existing mapping even
        // if another thread occupies a candidate after the snapshot was read.
        for (unsigned attempt{}; attempt < 3; ++attempt) {
            std::ifstream mapsFile("/proc/self/maps");
            if (!mapsFile.is_open()) [[unlikely]]
                throw exception("Failed to open /proc/self/maps");
            auto candidates{FindMappingCandidates(mapsFile, minSize, align, minAddress, maxAddress, findLargest)};
            for (auto [address, size] : candidates) {
                auto requested{reinterpret_cast<void *>(address)};
                auto result{mmap(requested, size, PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0)};
                if (result == MAP_FAILED) {
                    lastError = errno;
                    continue;
                }
                if (result == requested)
                    return {static_cast<u8 *>(result), size};
                // A collision (or a kernel placement policy) moved the mapping.
                // Release only the mapping returned by this call, then try again.
                if (munmap(result, size)) [[unlikely]]
                    throw exception("Failed to release relocated VMM reservation: {}", strerror(errno));
            }
        }
        throw exception("Could not reserve guest address space after 3 attempts (last mmap errno: {})", lastError);
    }

    namespace {
        constexpr size_t RegionAlignment{1ULL << 21}; //!< The minimum alignment of a HOS memory region

        namespace AS36bit {
            constexpr size_t CodeRegionStart{0x8000000}; //!< The start address of the code region (128MiB)
            constexpr size_t CodeRegionSize{0x78000000}; //!< The size of the code region (2GiB - 128MiB)
            constexpr size_t AliasRegionSize{0x180000000}; //!< The size of the alias region (6GiB)
            constexpr size_t HeapRegionSize{0x180000000}; //!< The size of the heap region (6GiB)

            constexpr size_t TotalSize{AliasRegionSize + HeapRegionSize};
        }

        namespace AS39bit {
            constexpr size_t MaxCodeRegionSize{4ULL * 1024 * 1024 * 1024}; //!< The assumed maximum size of the code region (4GiB)
            constexpr size_t AliasRegionSize{0x1000000000}; //!< The size of the alias region (64GiB)
            constexpr size_t HeapRegionSize{0x180000000}; //!< The size of the heap region (6GiB)
            constexpr size_t StackRegionSize{0x80000000}; //!< The size of the stack region (2GiB)
            constexpr size_t TlsIoRegionSize{0x1000000000}; //!< The size of the TLS/IO region (64GiB)

            constexpr size_t TotalSize{MaxCodeRegionSize + AliasRegionSize + HeapRegionSize + StackRegionSize + TlsIoRegionSize};
        }
    }

    void MemoryManager::InitializeVmm(memory::AddressSpaceType type) {
        addressSpaceType = type;

        LOGD("Initializing VMM for {}", to_string(addressSpaceType));

        size_t baseSize{}, maxAddress{};
        switch (type) {
            case memory::AddressSpaceType::AddressSpace32Bit:
            case memory::AddressSpaceType::AddressSpace32BitNoReserved:
                throw exception("32-bit address spaces are not supported");

            case memory::AddressSpaceType::AddressSpace36Bit: {
                addressSpace = span<u8>{reinterpret_cast<u8 *>(0), (1ULL << 36)};
                baseSize = AS36bit::TotalSize;
                maxAddress = addressSpace.size();
                break;
            }

            case memory::AddressSpaceType::AddressSpace39Bit: {
                addressSpace = span<u8>{reinterpret_cast<u8 *>(0), 1ULL << 39};
                baseSize = AS39bit::TotalSize;
                maxAddress = addressSpace.size();
                break;
            }

            default:
                throw exception("VMM initialization with unknown address space");
        }

        // Qualcomm KGSL (Kernel Graphic Support Layer/Kernel GPU driver) maps below 35-bits, reserving it causes KGSL to go OOM
        static constexpr size_t KgslReservedRegionSize{1ULL << 35};

        base = AllocateMappedRange(baseSize, RegionAlignment, KgslReservedRegionSize, maxAddress, false);

        switch (type) {
            case memory::AddressSpaceType::AddressSpace36Bit: {
                code = codeBase36Bit = AllocateMappedRange(AS36bit::CodeRegionSize, RegionAlignment, AS36bit::CodeRegionStart, KgslReservedRegionSize, false);

                if ((reinterpret_cast<u64>(base.data()) + baseSize) > (1ULL << 36)) {
                    LOGW("Couldn't fit regions into 36 bit AS! Resizing AS to 39 bits!");
                    addressSpace = span<u8>{reinterpret_cast<u8 *>(0), 1ULL << 39};
                }

                break;
            }

            default: {
                // Will be resized later when initializing regions
                code = base;
                break;
            }
        }

        // Insert a placeholder element at the end of the map to make sure upper_bound/lower_bound never triggers std::map::end() which is broken
        chunks = {{addressSpace.data(), {
            .size = addressSpace.size(),
            .state = memory::states::Unmapped,
        }}, {addressSpace.end().base(), {
            .state = memory::states::Reserved,
        }}};
    }

    void MemoryManager::InitializeRegions(span<u8> codeRegion) {
        // Get the host address of the code region
        codeRegion = GetHostSpan(codeRegion);

        if (!util::IsAligned(codeRegion.data(), RegionAlignment)) [[unlikely]]
            throw exception("Non-aligned code region was used to initialize regions: {} - {}", fmt::ptr(codeRegion.data()), fmt::ptr(codeRegion.end().base()));

        switch (addressSpaceType) {
            case memory::AddressSpaceType::AddressSpace36Bit: {
                // rtld expects the prefix to appear as inaccessible heap. It is
                // not an owned host reservation, so only publish its guest metadata.
                if (codeBase36Bit.data() != reinterpret_cast<u8 *>(AS36bit::CodeRegionStart)) {
                    MapInternal(std::pair<u8 *, ChunkDescriptor>(reinterpret_cast<u8 *>(AS36bit::CodeRegionStart), {
                        .size = reinterpret_cast<size_t>(codeBase36Bit.data() - AS36bit::CodeRegionStart),
                        .state = memory::states::Heap
                    }), false);
                }

                // Place code, stack and TLS/IO in the lower 36-bits of the host AS and heap and alias past that
                code = span<u8>{codeBase36Bit.data(), codeBase36Bit.data() + AS36bit::CodeRegionSize};
                stack = code; // stack is shared with code on 36-bit
                tlsIo = stack; // TLS/IO is shared with stack on 36-bit
                alias = span<u8>{base.data(), AS36bit::AliasRegionSize};
                heap = span<u8>{alias.host.end().base(), AS36bit::HeapRegionSize};
                break;
            }

            case memory::AddressSpaceType::AddressSpace39Bit: {
                code = span<u8>{base.data(), util::AlignUp(codeRegion.size(), RegionAlignment)};
                alias = span<u8>{code.host.end().base(), AS39bit::AliasRegionSize};
                heap = span<u8>{alias.host.end().base(), AS39bit::HeapRegionSize};
                stack = span<u8>{heap.host.end().base(), AS39bit::StackRegionSize};
                tlsIo = span<u8>{stack.host.end().base(), AS39bit::TlsIoRegionSize};

                u64 newSize{code.size() + alias.size() + stack.size() + heap.size() + tlsIo.size()};

                if (newSize > base.size()) [[unlikely]]
                    throw exception("Guest VMM size has exceeded host carveout size: 0x{:X}/0x{:X} (Code: 0x{:X}/0x{:X})", newSize, base.size(), code.size(), AS39bit::MaxCodeRegionSize);

                if (newSize != base.size()) [[likely]] {
                    if (munmap(base.data() + newSize, base.size() - newSize)) [[unlikely]]
                        throw exception("Failed to shrink guest VMM reservation: {}", strerror(errno));
                    base = span<u8>{base.data(), newSize};
                }

                break;
            }

            default:
                throw exception("Regions initialized without VMM initialization");
        }

        if (codeRegion.size() > code.size()) [[unlikely]]
            throw exception("Code region ({}) is smaller than mapped code size ({})", code.size(), codeRegion.size());

        LOGD("Region Map:\n"
             "VMM Base: {}\n"
             "Code Region: {} - {} (Size: 0x{:X})\n"
             "Alias Region: {} - {} (Size: 0x{:X})\n"
             "Heap Region: {} - {} (Size: 0x{:X})\n"
             "Stack Region: {} - {} (Size: 0x{:X})\n"
             "TLS/IO Region: {} - {} (Size: 0x{:X})",
             fmt::ptr(base.data()),
             fmt::ptr(code.guest.data()), fmt::ptr(code.guest.end().base()), code.size(),
             fmt::ptr(alias.guest.data()), fmt::ptr(alias.guest.end().base()), alias.size(),
             fmt::ptr(heap.guest.data()), fmt::ptr(heap.guest.end().base()), heap.size(),
             fmt::ptr(stack.guest.data()), fmt::ptr(stack.guest.end().base()), stack.size(),
             fmt::ptr(tlsIo.guest.data()), fmt::ptr(tlsIo.guest.end().base()), tlsIo.size());
    }

    span<u8> MemoryManager::CreateMirror(span<u8> mapping) {
        // Get the host address of the mapping
        mapping = GetHostSpan(mapping);

        if (!mapping.size() || !base.contains(mapping)) [[unlikely]]
            throw exception("Mapping is outside of VMM base: {} - {}", fmt::ptr(mapping.data()), fmt::ptr(mapping.end().base()));

        auto offset{static_cast<size_t>(mapping.data() - base.data())};
        if (!util::IsPageAligned(offset) || !util::IsPageAligned(mapping.size())) [[unlikely]]
            throw exception("Mapping is not aligned to a page: {} - {} (0x{:X})", fmt::ptr(mapping.data()), fmt::ptr(mapping.end().base()), offset);

        auto mirror{mremap(mapping.data(), 0, mapping.size(), MREMAP_MAYMOVE)};
        if (mirror == MAP_FAILED) [[unlikely]]
            throw exception("Failed to create mirror mapping at {} - {} (0x{:X}): {}", fmt::ptr(mapping.data()), fmt::ptr(mapping.end().base()), offset, strerror(errno));

        if (mprotect(mirror, mapping.size(), PROT_READ | PROT_WRITE)) [[unlikely]] {
            int error{errno};
            if (munmap(mirror, mapping.size()))
                LOGE("Failed to release unusable mirror mapping: {}", strerror(errno));
            throw exception("Failed to protect mirror mapping: {}", strerror(error));
        }

        return span<u8>{reinterpret_cast<u8 *>(mirror), mapping.size()};
    }

    span<u8> MemoryManager::CreateMirrors(const std::vector<span<u8>> &regions) {
        size_t totalSize{};
        // Validate every region before reserving or creating any mapping.
        for (const auto &region : regions) {
            auto hostRegion{GetHostSpan(region)};
            if (!hostRegion.size() || !base.contains(hostRegion)) [[unlikely]]
                throw exception("Mirror region is empty or outside of VMM base");
            if (!util::IsPageAligned(hostRegion.data()) || !util::IsPageAligned(hostRegion.size())) [[unlikely]]
                throw exception("Mirror region is not page aligned");
            if (region.size() > std::numeric_limits<size_t>::max() - totalSize) [[unlikely]]
                throw exception("Combined mirror size overflows size_t");
            totalSize += region.size();
        }
        if (!totalSize) [[unlikely]]
            throw exception("Cannot create an empty mirror mapping");

        auto mirrorBase{mmap(nullptr, totalSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}; // Reserve address space for all mirrors
        if (mirrorBase == MAP_FAILED) [[unlikely]]
            throw exception("Failed to create mirror base: {} (0x{:X} bytes)", strerror(errno), totalSize);

        try {
            size_t mirrorOffset{};
            for (const auto &region : regions) {
                auto hostRegion{GetHostSpan(region)};
                auto mirror{mremap(hostRegion.data(), 0, hostRegion.size(), MREMAP_FIXED | MREMAP_MAYMOVE, reinterpret_cast<u8 *>(mirrorBase) + mirrorOffset)};
                if (mirror == MAP_FAILED) [[unlikely]]
                    throw exception("Failed to create mirror mapping: {}", strerror(errno));
                if (mprotect(mirror, hostRegion.size(), PROT_READ | PROT_WRITE)) [[unlikely]]
                    throw exception("Failed to protect mirror mapping: {}", strerror(errno));
                mirrorOffset += hostRegion.size();
            }
        } catch (...) {
            // Includes both completed mirrors and the untouched reservation tail.
            if (munmap(mirrorBase, totalSize))
                LOGE("Failed to release incomplete mirror mapping: {}", strerror(errno));
            throw;
        }

        return span<u8>{reinterpret_cast<u8 *>(mirrorBase), totalSize};
    }

    void MemoryManager::SetRegionBorrowed(span<u8> memory, bool value) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.attributes.isBorrowed = value;
            MapInternal(desc);
        });
    }

    void MemoryManager::SetRegionCpuCaching(span<u8> memory, bool value) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.attributes.isUncached = value;
            MapInternal(desc);
        });
    }

    void MemoryManager::SetRegionPermission(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.permission = permission;
            MapInternal(desc);
        });
    }

    std::optional<std::pair<u8 *, ChunkDescriptor>> MemoryManager::GetChunk(u8 *addr) {
        std::shared_lock lock{mutex};

        if (!addressSpace.contains(addr)) [[unlikely]]
            return std::nullopt;

        auto chunkBase{chunks.lower_bound(addr)};
        if (addr < chunkBase->first)
            --chunkBase;

        return std::make_optional(*chunkBase);
    }

    __attribute__((always_inline)) void MemoryManager::MapCodeMemory(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = permission,
                .state = memory::states::Code
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapMutableCodeMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::CodeMutable
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapStackMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::Stack,
                .isSrcMergeDisallowed = true
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapHeapMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::Heap
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapSharedMemory(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = permission,
                .state = memory::states::SharedMemory,
                .isSrcMergeDisallowed = true
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapTransferMemory(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = permission,
                .state = permission.raw ? memory::states::TransferMemory : memory::states::TransferMemoryIsolated,
                .isSrcMergeDisallowed = true
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapThreadLocalMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::ThreadLocal
            }));
    }

    __attribute__((always_inline)) void MemoryManager::Reserve(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {false, false, false},
                .state = memory::states::Reserved
            }));
    }

    __attribute__((always_inline)) void MemoryManager::UnmapMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](const std::pair<u8 *, ChunkDescriptor> &desc) {
            if (desc.second.state != memory::states::Unmapped)
                FreeMemory(span<u8>(desc.first, desc.second.size));
        });

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {false, false, false},
                .state = memory::states::Unmapped
            }));
    }

    __attribute__((always_inline)) void MemoryManager::FreeMemory(span<u8> memory) {
        // Get the host address of memory
        memory = GetHostSpan(memory);

        u8 *alignedStart{util::AlignUp(memory.data(), constant::PageSize)};
        u8 *alignedEnd{util::AlignDown(memory.end().base(), constant::PageSize)};

        if (alignedStart < alignedEnd) [[likely]]
            if (madvise(alignedStart, static_cast<size_t>(alignedEnd - alignedStart), MADV_REMOVE) == -1) [[unlikely]]
                LOGE("Failed to free memory: {}", strerror(errno));
    }

    void MemoryManager::SvcMapMemory(span<u8> source, span<u8> destination) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            destination.data(), {
                .size = destination.size(),
                .permission = {true, true, false},
                .state = memory::states::Stack,
                .isSrcMergeDisallowed = true
            }));

        auto sourceHost{GetHostSpan(source)};
        auto destinationHost{GetHostSpan(destination)};
        std::memcpy(destinationHost.data(), sourceHost.data(), sourceHost.size());

        ForeachChunkInRange(source, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.permission = {false, false, false};
            desc.second.attributes.isBorrowed = true;
            MapInternal(desc);
        });
    }

    void MemoryManager::SvcUnmapMemory(span<u8> source, span<u8> destination) {
        std::unique_lock lock{mutex};

        if (source.size() != destination.size()) [[unlikely]]
            throw exception("Mismatched svcUnmapMemory range sizes");

        ForeachChunkInRange(destination, [&](const std::pair<u8 *, ChunkDescriptor> &dstChunk) {
            if (dstChunk.second.state == memory::states::Unmapped)
                return;

            size_t offset{static_cast<size_t>(dstChunk.first - destination.data())};
            span<u8> sourcePart{source.data() + offset, dstChunk.second.size};
            ForeachChunkInRange(sourcePart, [&](std::pair<u8 *, ChunkDescriptor> &desc) {
                desc.second.permission = dstChunk.second.permission;
                desc.second.attributes.isBorrowed = false;
                MapInternal(desc);
            });

            auto sourceHost{GetHostSpan(sourcePart)};
            auto destinationHost{GetHostSpan({dstChunk.first, dstChunk.second.size})};
            std::memcpy(sourceHost.data(), destinationHost.data(), sourceHost.size());
        });
    }

    void MemoryManager::AddRef(std::shared_ptr<type::KMemory> ptr) {
        std::lock_guard lock{memRefsMutex};
        memRefs.push_back(std::move(ptr));
    }

    void MemoryManager::RemoveRef(const std::shared_ptr<type::KMemory> &ptr) {
        // Release the object outside the collection lock: its destructor may
        // re-enter memory management.
        std::shared_ptr<type::KMemory> removed;
        {
            std::lock_guard lock{memRefsMutex};
            auto i = std::find(memRefs.begin(), memRefs.end(), ptr);
            if (i == memRefs.end())
                return;
            removed = std::move(*i);
            memRefs.erase(i);
        }
    }

    size_t MemoryManager::GetUserMemoryUsage() {
        std::shared_lock lock{mutex};
        size_t size{};

        auto currChunk = chunks.lower_bound(heap.guest.data());

        while (currChunk->first < heap.guest.end().base()) {
            if (currChunk->second.state == memory::states::Heap)
                size += currChunk->second.size;
            ++currChunk;
        }

        return size + code.size() + state.process->mainThreadStack.size();
    }

    size_t MemoryManager::GetSystemResourceUsage() {
        std::shared_lock lock{mutex};
        constexpr size_t KMemoryBlockSize{0x40};
        return std::min(static_cast<size_t>(state.process->npdm.meta.systemResourceSize), util::AlignUp(chunks.size() * KMemoryBlockSize, constant::PageSize));
    }

    __attribute__((always_inline)) span<u8> MemoryManager::GetHostSpan(span<u8> guestSpan) const {
        return {guestSpan.data() + guestOffset, guestSpan.size()};
    }
}
