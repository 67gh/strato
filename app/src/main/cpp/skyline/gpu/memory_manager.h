// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Optimized for MediaTek (ARM Mali) GPUs — reduced RAM consumption, lazy mapping, pool recycling

#pragma once

#include <vk_mem_alloc.h>
#include "fence_cycle.h"

namespace skyline::gpu::memory {

    // -------------------------------------------------------------------------
    // Vendor IDs for GPU-specific paths
    // -------------------------------------------------------------------------
    constexpr u32 kVendorARM       = 0x13B5; // ARM Mali (MediaTek Dimensity, etc.)
    constexpr u32 kVendorQualcomm  = 0x5143; // Qualcomm Adreno
    constexpr u32 kVendorNvidia    = 0x10DE;
    constexpr u32 kVendorAMD       = 0x1002;

    /**
     * @brief A view into a CPU mapping of a Vulkan buffer
     * @note The mapping **should not** be used after the lifetime of the object has ended
     */
    struct Buffer : public span<u8> {
        VmaAllocator vmaAllocator;
        VmaAllocation vmaAllocation;
        vk::Buffer vkBuffer;

        constexpr Buffer(u8 *pointer, size_t size, VmaAllocator vmaAllocator, vk::Buffer vkBuffer, VmaAllocation vmaAllocation)
            : vmaAllocator(vmaAllocator),
              vkBuffer(vkBuffer),
              vmaAllocation(vmaAllocation),
              span(pointer, size) {}

        Buffer(const Buffer &) = delete;

        constexpr Buffer(Buffer &&other)
            : vmaAllocator(std::exchange(other.vmaAllocator, nullptr)),
              vmaAllocation(std::exchange(other.vmaAllocation, nullptr)),
              vkBuffer(std::exchange(other.vkBuffer, {})),
              span(other) {}

        Buffer &operator=(const Buffer &) = delete;
        Buffer &operator=(Buffer &&) = default;

        ~Buffer();
    };

    /**
     * @brief A Buffer that can be independently attached to a fence cycle
     */
    class StagingBuffer : public Buffer {
        using Buffer::Buffer;
    };

    /**
     * @brief A buffer that directly owns its own memory
     */
    struct ImportedBuffer : public span<u8> {
        vk::raii::Buffer vkBuffer;
        vk::raii::DeviceMemory vkMemory;

        ImportedBuffer(span<u8> data, vk::raii::Buffer vkBuffer, vk::raii::DeviceMemory vkMemory)
            : vkBuffer{std::move(vkBuffer)},
              vkMemory{std::move(vkMemory)},
              span{data} {}

        ImportedBuffer(const ImportedBuffer &) = delete;

        ImportedBuffer(ImportedBuffer &&other)
            : vkBuffer{std::move(other.vkBuffer)},
              vkMemory{std::move(other.vkMemory)},
              span{other} {}

        ImportedBuffer &operator=(const ImportedBuffer &) = delete;
        ImportedBuffer &operator=(ImportedBuffer &&) = default;
    };

    /**
     * @brief A Vulkan image managed by VMA
     * @note Images created with VMA_ALLOCATION_CREATE_MAPPED_BIT must not use this path
     *       because unmapping is handled automatically on destruction.
     */
    struct Image {
      private:
        u8 *pointer{};

      public:
        VmaAllocator vmaAllocator;
        VmaAllocation vmaAllocation;
        vk::Image vkImage;

        constexpr Image(VmaAllocator vmaAllocator, vk::Image vkImage, VmaAllocation vmaAllocation)
            : vmaAllocator(vmaAllocator),
              vkImage(vkImage),
              vmaAllocation(vmaAllocation) {}

        constexpr Image(u8 *pointer, VmaAllocator vmaAllocator, vk::Image vkImage, VmaAllocation vmaAllocation)
            : pointer(pointer),
              vmaAllocator(vmaAllocator),
              vkImage(vkImage),
              vmaAllocation(vmaAllocation) {}

        Image(const Image &) = delete;

        constexpr Image(Image &&other)
            : pointer(std::exchange(other.pointer, nullptr)),
              vmaAllocator(std::exchange(other.vmaAllocator, nullptr)),
              vmaAllocation(std::exchange(other.vmaAllocation, nullptr)),
              vkImage(std::exchange(other.vkImage, {})) {}

        Image &operator=(const Image &) = delete;
        Image &operator=(Image &&) = default;

        ~Image();

        /**
         * @return A pointer to a CPU mapping of the image.
         * @note Lazy — only maps on first call (saves RAM when image is GPU-only).
         */
        u8 *data();

        /**
         * @brief Explicitly unmap the image from CPU address space to free virtual memory.
         *        Useful on MediaTek where IOMMU pressure is high.
         */
        void Unmap();
    };

    // -------------------------------------------------------------------------
    // Staging buffer pool
    // Avoids repeated vmaCreateBuffer / vmaDestroyBuffer on MediaTek where
    // IOMMU TLB shootdowns are expensive.
    // -------------------------------------------------------------------------

    /**
     * @brief A pooled, ref-counted staging buffer entry
     */
    struct StagingBufferEntry {
        std::shared_ptr<StagingBuffer> buffer;
        bool inUse{false};
    };

    /**
     * @brief An abstraction over memory operations done in Vulkan, used for all
     *        allocations on the host GPU.
     *
     * Key improvements over the original:
     *  - VMA flags tuned per GPU vendor (ARM Mali / Qualcomm Adreno / generic).
     *  - Staging buffer pool: re-uses allocations instead of destroy/recreate.
     *  - AllocateBuffer falls back gracefully when device-local+host-visible
     *    memory is unavailable (common on low-end MediaTek).
     *  - AllocateMappedImage defers CPU mapping to first access (lazy mapping).
     *  - Explicit Trim() to release VMA internal free blocks under memory pressure.
     */
    class MemoryManager {
      private:
        GPU &gpu;
        VmaAllocator vmaAllocator{VK_NULL_HANDLE};

        // Staging pool — keyed by buffer size bucket (power-of-two bucketing).
        std::mutex poolMutex;
        std::vector<StagingBufferEntry> stagingPool;

        static constexpr size_t kMaxPoolEntries = 8; //!< Keep at most 8 idle staging buffers

        /**
         * @brief Round size up to the nearest power-of-two bucket (min 64 KiB).
         *        This reduces pool fragmentation and improves hit rate.
         */
        static vk::DeviceSize BucketSize(vk::DeviceSize size);

        /**
         * @brief Returns true if the physical device is an ARM Mali GPU
         *        (present in all MediaTek Dimensity and older Helio SoCs).
         */
        [[nodiscard]] bool IsMaliGpu() const;

        /**
         * @brief Returns true if the device can allocate host-visible device-local memory.
         *        On many MediaTek SoCs this memory type does not exist.
         */
        [[nodiscard]] bool HasDeviceLocalHostVisibleMemory() const;

      public:
        explicit MemoryManager(GPU &gpu);
        ~MemoryManager();

        /**
         * @brief Creates a buffer optimised for staging (Transfer Src/Dst).
         *        On Mali, returns a pooled buffer when possible to avoid
         *        repeated IOMMU map/unmap cycles.
         */
        std::shared_ptr<StagingBuffer> AllocateStagingBuffer(vk::DeviceSize size);

        /**
         * @brief Releases a staging buffer back to the internal pool so it can
         *        be reused by the next AllocateStagingBuffer call of the same
         *        or smaller size.
         * @note  Call this instead of letting the shared_ptr expire when you
         *        know the buffer will not be used by the GPU anymore.
         */
        void ReleaseStagingBuffer(std::shared_ptr<StagingBuffer> buffer);

        /**
         * @brief Creates a buffer with a CPU mapping and all relevant usage flags.
         *        Falls back to CPU-only memory when device-local+host-visible
         *        is unavailable (graceful degradation for low-end MediaTek).
         */
        Buffer AllocateBuffer(vk::DeviceSize size);

        /**
         * @brief Creates an image allocated and deallocated using RAII.
         *        On Mali, sets VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT to
         *        reduce fragmentation in the IOMMU address space.
         */
        Image AllocateImage(const vk::ImageCreateInfo &createInfo);

        /**
         * @brief Creates a CPU-mappable image using RAII with lazy mapping.
         *        The CPU pointer is only established on first Image::data() call.
         */
        Image AllocateMappedImage(const vk::ImageCreateInfo &createInfo);

        /**
         * @brief Maps the input CPU region into a new Vulkan buffer.
         *        Only available when adrenotools direct memory import is supported.
         */
        ImportedBuffer ImportBuffer(span<u8> cpuMapping);

        /**
         * @brief Advances the VMA frame index so it can reclaim unused internal
         *        free blocks. Compatible with VMA 2.x and 3.x.
         *        Call this when a game's loading screen ends or under low-memory
         *        pressure. Particularly effective on Mali where the kernel
         *        allocator retains large slabs.
         */
        void Trim();
    };
}
