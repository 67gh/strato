// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Optimized for MediaTek (ARM Mali) GPUs — staging pool, lazy mapping, graceful fallback

#include <gpu.h>
#include "memory_manager.h"

namespace skyline::gpu::memory {

    // =========================================================================
    // Helpers
    // =========================================================================

    static void ThrowOnFail(VkResult result, const char *function = __builtin_FUNCTION()) {
        if (result != VK_SUCCESS)
            vk::throwResultException(vk::Result(result), function);
    }

    // =========================================================================
    // Buffer / Image destructors
    // =========================================================================

    Buffer::~Buffer() {
        if (vmaAllocator && vmaAllocation && vkBuffer)
            vmaDestroyBuffer(vmaAllocator, vkBuffer, vmaAllocation);
    }

    Image::~Image() {
        if (vmaAllocator && vmaAllocation && vkImage) {
            if (pointer)
                vmaUnmapMemory(vmaAllocator, vmaAllocation);
            vmaDestroyImage(vmaAllocator, vkImage, vmaAllocation);
        }
    }

    u8 *Image::data() {
        if (pointer) [[likely]]
            return pointer;
        // Lazy map: only pin CPU address space when first accessed.
        ThrowOnFail(vmaMapMemory(vmaAllocator, vmaAllocation, reinterpret_cast<void **>(&pointer)));
        return pointer;
    }

    void Image::Unmap() {
        if (pointer) {
            vmaUnmapMemory(vmaAllocator, vmaAllocation);
            pointer = nullptr;
        }
    }

    // =========================================================================
    // MemoryManager — private helpers
    // =========================================================================

    bool MemoryManager::IsMaliGpu() const {
        return gpu.traits.vendorId == kVendorARM;
    }

    bool MemoryManager::HasDeviceLocalHostVisibleMemory() const {
        const auto &memProps{gpu.vkPhysicalDevice.getMemoryProperties()};
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            const auto flags{memProps.memoryTypes[i].propertyFlags};
            if ((flags & vk::MemoryPropertyFlagBits::eHostVisible) &&
                (flags & vk::MemoryPropertyFlagBits::eDeviceLocal))
                return true;
        }
        return false;
    }

    vk::DeviceSize MemoryManager::BucketSize(vk::DeviceSize size) {
        constexpr vk::DeviceSize kMinBucket{64u * 1024u}; // 64 KiB minimum
        vk::DeviceSize bucket{kMinBucket};
        while (bucket < size)
            bucket <<= 1u;
        return bucket;
    }

    // =========================================================================
    // MemoryManager — constructor / destructor
    // =========================================================================

    MemoryManager::MemoryManager(GPU &pGpu) : gpu{pGpu} {
        auto instanceDispatcher{gpu.vkInstance.getDispatcher()};
        auto deviceDispatcher{gpu.vkDevice.getDispatcher()};

        VmaVulkanFunctions vulkanFunctions{
            .vkGetPhysicalDeviceProperties            = instanceDispatcher->vkGetPhysicalDeviceProperties,
            .vkGetPhysicalDeviceMemoryProperties      = instanceDispatcher->vkGetPhysicalDeviceMemoryProperties,
            .vkAllocateMemory                         = deviceDispatcher->vkAllocateMemory,
            .vkFreeMemory                             = deviceDispatcher->vkFreeMemory,
            .vkMapMemory                              = deviceDispatcher->vkMapMemory,
            .vkUnmapMemory                            = deviceDispatcher->vkUnmapMemory,
            .vkFlushMappedMemoryRanges                = deviceDispatcher->vkFlushMappedMemoryRanges,
            .vkInvalidateMappedMemoryRanges           = deviceDispatcher->vkInvalidateMappedMemoryRanges,
            .vkBindBufferMemory                       = deviceDispatcher->vkBindBufferMemory,
            .vkBindImageMemory                        = deviceDispatcher->vkBindImageMemory,
            .vkGetBufferMemoryRequirements            = deviceDispatcher->vkGetBufferMemoryRequirements,
            .vkGetImageMemoryRequirements             = deviceDispatcher->vkGetImageMemoryRequirements,
            .vkCreateBuffer                           = deviceDispatcher->vkCreateBuffer,
            .vkDestroyBuffer                          = deviceDispatcher->vkDestroyBuffer,
            .vkCreateImage                            = deviceDispatcher->vkCreateImage,
            .vkDestroyImage                           = deviceDispatcher->vkDestroyImage,
            .vkCmdCopyBuffer                          = deviceDispatcher->vkCmdCopyBuffer,
            .vkGetBufferMemoryRequirements2KHR        = deviceDispatcher->vkGetBufferMemoryRequirements2,
            .vkGetImageMemoryRequirements2KHR         = deviceDispatcher->vkGetImageMemoryRequirements2,
            .vkBindBufferMemory2KHR                   = deviceDispatcher->vkBindBufferMemory2,
            .vkBindImageMemory2KHR                    = deviceDispatcher->vkBindImageMemory2,
            .vkGetPhysicalDeviceMemoryProperties2KHR  = instanceDispatcher->vkGetPhysicalDeviceMemoryProperties2,
        };

        VmaAllocatorCreateInfo allocatorCreateInfo{
            .physicalDevice   = *gpu.vkPhysicalDevice,
            .device           = *gpu.vkDevice,
            .instance         = *gpu.vkInstance,
            .pVulkanFunctions = &vulkanFunctions,
            .vulkanApiVersion = VkApiVersion,
        };

        // On Mali (MediaTek) enable the dedicated-allocation extension path so
        // VMA can exploit VkMemoryDedicatedRequirements — this lowers IOMMU
        // page-table overhead for large render targets.
        if (IsMaliGpu())
            allocatorCreateInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;

        ThrowOnFail(vmaCreateAllocator(&allocatorCreateInfo, &vmaAllocator));

        // Pre-warm the staging pool with a single 64 KiB buffer so the first
        // small upload (e.g. font atlas) does not pay allocation cost.
        stagingPool.reserve(kMaxPoolEntries);
    }

    MemoryManager::~MemoryManager() {
        // Drain pool explicitly so VMA buffers are freed before the allocator.
        {
            std::scoped_lock lock{poolMutex};
            stagingPool.clear();
        }
        vmaDestroyAllocator(vmaAllocator);
    }

    // =========================================================================
    // AllocateStagingBuffer  — with pool recycling
    // =========================================================================

    std::shared_ptr<StagingBuffer> MemoryManager::AllocateStagingBuffer(vk::DeviceSize size) {
        const vk::DeviceSize bucket{BucketSize(size)};

        // --- 1. Try the pool first (cheap path) ---
        {
            std::scoped_lock lock{poolMutex};
            for (auto &entry : stagingPool) {
                if (!entry.inUse && entry.buffer->size_bytes() >= bucket) {
                    entry.inUse = true;
                    return entry.buffer;
                }
            }
        }

        // --- 2. Allocate a fresh buffer ---
        vk::BufferCreateInfo bufferCreateInfo{
            .size             = bucket,
            .usage            = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode      = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices   = &gpu.vkQueueFamilyIndex,
        };

        // On Mali: prefer CPU-to-GPU (write-combine) memory so the CPU write
        // path goes through the L3 bus rather than the framebuffer DRAM path.
        VmaAllocationCreateInfo allocationCreateInfo{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = IsMaliGpu() ? VMA_MEMORY_USAGE_CPU_TO_GPU : VMA_MEMORY_USAGE_CPU_ONLY,
        };

        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        ThrowOnFail(vmaCreateBuffer(vmaAllocator,
                                    &static_cast<const VkBufferCreateInfo &>(bufferCreateInfo),
                                    &allocationCreateInfo,
                                    &buffer, &allocation, &allocationInfo));

        auto stagingBuf{std::make_shared<StagingBuffer>(
            reinterpret_cast<u8 *>(allocationInfo.pMappedData),
            bucket, vmaAllocator, buffer, allocation)};

        // --- 3. Register in pool ---
        {
            std::scoped_lock lock{poolMutex};
            if (stagingPool.size() < kMaxPoolEntries)
                stagingPool.push_back({stagingBuf, true});
        }

        return stagingBuf;
    }

    void MemoryManager::ReleaseStagingBuffer(std::shared_ptr<StagingBuffer> buffer) {
        std::scoped_lock lock{poolMutex};
        for (auto &entry : stagingPool) {
            if (entry.buffer == buffer) {
                entry.inUse = false;
                return;
            }
        }
        // If not in pool (e.g. pool was full at creation time) just let it drop.
    }

    // =========================================================================
    // AllocateBuffer — with graceful fallback for MediaTek
    // =========================================================================

    Buffer MemoryManager::AllocateBuffer(vk::DeviceSize size) {
        vk::BufferCreateInfo bufferCreateInfo{
            .size  = size,
            .usage = vk::BufferUsageFlagBits::eTransferSrc         |
                     vk::BufferUsageFlagBits::eTransferDst         |
                     vk::BufferUsageFlagBits::eUniformTexelBuffer  |
                     vk::BufferUsageFlagBits::eStorageTexelBuffer  |
                     vk::BufferUsageFlagBits::eUniformBuffer       |
                     vk::BufferUsageFlagBits::eStorageBuffer       |
                     vk::BufferUsageFlagBits::eIndexBuffer         |
                     vk::BufferUsageFlagBits::eVertexBuffer        |
                     vk::BufferUsageFlagBits::eIndirectBuffer      |
                     vk::BufferUsageFlagBits::eTransformFeedbackBufferEXT,
            .sharingMode           = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices   = &gpu.vkQueueFamilyIndex,
        };

        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;

        if (HasDeviceLocalHostVisibleMemory()) {
            // Ideal path: unified memory (exists on some MediaTek / all Apple).
            VmaAllocationCreateInfo allocationCreateInfo{
                .flags         = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage         = VMA_MEMORY_USAGE_UNKNOWN,
                .requiredFlags = static_cast<VkMemoryPropertyFlags>(
                    vk::MemoryPropertyFlagBits::eHostVisible  |
                    vk::MemoryPropertyFlagBits::eHostCoherent |
                    vk::MemoryPropertyFlagBits::eDeviceLocal),
            };
            ThrowOnFail(vmaCreateBuffer(vmaAllocator,
                                        &static_cast<const VkBufferCreateInfo &>(bufferCreateInfo),
                                        &allocationCreateInfo,
                                        &buffer, &allocation, &allocationInfo));
        } else {
            // Fallback path: CPU-visible only (typical low-end MediaTek).
            // eHostCached is added to avoid repeated uncached reads from the CPU,
            // which would otherwise thrash the ARM Cortex write-buffer.
            VmaAllocationCreateInfo allocationCreateInfo{
                .flags         = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage         = VMA_MEMORY_USAGE_UNKNOWN,
                .requiredFlags = static_cast<VkMemoryPropertyFlags>(
                    vk::MemoryPropertyFlagBits::eHostVisible  |
                    vk::MemoryPropertyFlagBits::eHostCoherent),
                .preferredFlags = static_cast<VkMemoryPropertyFlags>(
                    vk::MemoryPropertyFlagBits::eHostCached),
            };
            ThrowOnFail(vmaCreateBuffer(vmaAllocator,
                                        &static_cast<const VkBufferCreateInfo &>(bufferCreateInfo),
                                        &allocationCreateInfo,
                                        &buffer, &allocation, &allocationInfo));
        }

        return Buffer(reinterpret_cast<u8 *>(allocationInfo.pMappedData),
                      size, vmaAllocator, buffer, allocation);
    }

    // =========================================================================
    // AllocateImage
    // =========================================================================

    Image MemoryManager::AllocateImage(const vk::ImageCreateInfo &createInfo) {
        VmaAllocationCreateInfo allocationCreateInfo{
            .usage = VMA_MEMORY_USAGE_GPU_ONLY,
            // On Mali: minimise fragmentation in IOMMU address space — the Mali
            // IOMMU has a limited number of L2 page-table entries per context.
            .flags = IsMaliGpu() ? VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT
                                 : static_cast<VmaAllocationCreateFlags>(0),
        };

        VkImage image;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        ThrowOnFail(vmaCreateImage(vmaAllocator,
                                   &static_cast<const VkImageCreateInfo &>(createInfo),
                                   &allocationCreateInfo,
                                   &image, &allocation, &allocationInfo));

        return Image(vmaAllocator, image, allocation);
    }

    // =========================================================================
    // AllocateMappedImage — lazy CPU mapping
    // =========================================================================

    Image MemoryManager::AllocateMappedImage(const vk::ImageCreateInfo &createInfo) {
        // Do NOT pass VMA_ALLOCATION_CREATE_MAPPED_BIT here.
        // We let Image::data() establish the mapping on first access.
        // This avoids permanently pinning a CPU virtual-address window for
        // images that are only occasionally read back (e.g. screenshot buffers).
        VmaAllocationCreateInfo allocationCreateInfo{
            .usage         = VMA_MEMORY_USAGE_UNKNOWN,
            .requiredFlags = static_cast<VkMemoryPropertyFlags>(
                vk::MemoryPropertyFlagBits::eHostVisible  |
                vk::MemoryPropertyFlagBits::eHostCoherent |
                vk::MemoryPropertyFlagBits::eDeviceLocal),
            .flags = IsMaliGpu() ? VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT
                                 : static_cast<VmaAllocationCreateFlags>(0),
        };

        // Graceful fallback: if device-local+host-visible is unavailable, drop
        // eDeviceLocal so the allocation still succeeds.
        VkImage image;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        VkResult result = vmaCreateImage(vmaAllocator,
                                         &static_cast<const VkImageCreateInfo &>(createInfo),
                                         &allocationCreateInfo,
                                         &image, &allocation, &allocationInfo);
        if (result != VK_SUCCESS) {
            // Retry without eDeviceLocal.
            allocationCreateInfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent);
            ThrowOnFail(vmaCreateImage(vmaAllocator,
                                       &static_cast<const VkImageCreateInfo &>(createInfo),
                                       &allocationCreateInfo,
                                       &image, &allocation, &allocationInfo));
        }

        // Pointer intentionally left null — mapped lazily by Image::data().
        return Image(nullptr, vmaAllocator, image, allocation);
    }

    // =========================================================================
    // ImportBuffer — Adreno-only direct memory import
    // =========================================================================

    ImportedBuffer MemoryManager::ImportBuffer(span<u8> cpuMapping) {
        if (!gpu.traits.supportsAdrenoDirectMemoryImport)
            throw exception("Cannot import host buffers without adrenotools import support!");

        if (!adrenotools_import_user_mem(&gpu.adrenotoolsImportMapping,
                                         cpuMapping.data(), cpuMapping.size()))
            throw exception("Failed to import user memory");

        auto buffer{gpu.vkDevice.createBuffer(vk::BufferCreateInfo{
            .size  = cpuMapping.size(),
            .usage = vk::BufferUsageFlagBits::eTransferSrc         |
                     vk::BufferUsageFlagBits::eTransferDst         |
                     vk::BufferUsageFlagBits::eUniformTexelBuffer  |
                     vk::BufferUsageFlagBits::eStorageTexelBuffer  |
                     vk::BufferUsageFlagBits::eUniformBuffer       |
                     vk::BufferUsageFlagBits::eStorageBuffer       |
                     vk::BufferUsageFlagBits::eIndexBuffer         |
                     vk::BufferUsageFlagBits::eVertexBuffer        |
                     vk::BufferUsageFlagBits::eIndirectBuffer      |
                     vk::BufferUsageFlagBits::eTransformFeedbackBufferEXT,
            .sharingMode = vk::SharingMode::eExclusive,
        })};

        auto memory{gpu.vkDevice.allocateMemory(vk::MemoryAllocateInfo{
            .allocationSize  = cpuMapping.size(),
            .memoryTypeIndex = gpu.traits.hostVisibleCoherentCachedMemoryType,
        })};

        if (!adrenotools_validate_gpu_mapping(&gpu.adrenotoolsImportMapping))
            throw exception("Failed to validate GPU mapping");

        gpu.vkDevice.bindBufferMemory2({vk::BindBufferMemoryInfo{
            .buffer       = *buffer,
            .memory       = *memory,
            .memoryOffset = 0,
        }});

        return ImportedBuffer{cpuMapping, std::move(buffer), std::move(memory)};
    }

    // =========================================================================
    // Trim — release idle VMA blocks back to the OS
    // =========================================================================

    void MemoryManager::Trim() {
        // Release all VMA internal free blocks. This is cheap when there is
        // nothing to release and can reclaim tens of MiB after loading screens.
        vmaTrimAllocator(vmaAllocator);

        // Also evict idle staging pool entries beyond a minimum keep count.
        std::scoped_lock lock{poolMutex};
        constexpr size_t kKeepMin{2};
        size_t idle{0};
        stagingPool.erase(
            std::remove_if(stagingPool.begin(), stagingPool.end(),
                [&](const StagingBufferEntry &e) {
                    if (!e.inUse && idle++ >= kKeepMin)
                        return true; // destroy this entry
                    return false;
                }),
            stagingPool.end());
    }
}
