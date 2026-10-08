// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <array>
#include <locale>
#include <sstream>
#include <vector>
#include <common/diagnostic_session.h>
#include "trait_manager.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace skyline::gpu::diagnostics {
    /**
     * A one-time snapshot taken after logical-device creation. No optional diagnostic
     * feature is enabled here. Budget and usage are driver estimates, not allocator accounting.
     */
    inline void WriteVulkanSnapshot(const vk::raii::PhysicalDevice &physicalDevice,
                                    const TraitManager::DeviceProperties2 &propertiesChain,
                                    TraitManager &traits,
                                    const std::vector<vk::ExtensionProperties> &supportedExtensions,
                                    const std::vector<std::array<char, VK_MAX_EXTENSION_NAME_SIZE>> &enabledExtensions,
                                    u32 queueFamilyIndex) noexcept {
        try {
            using skyline::diagnostics::JsonEscape;
            auto quote{[](std::string_view value) { return '"' + JsonEscape(value) + '"'; }};
            auto supported{[&](std::string_view name) {
                return std::any_of(supportedExtensions.begin(), supportedExtensions.end(), [&](const auto &extension) {
                    return std::string_view{extension.extensionName} == name;
                });
            }};
            auto enabled{[&](std::string_view name) {
                return std::any_of(enabledExtensions.begin(), enabledExtensions.end(), [&](const auto &extension) {
                    return std::string_view{extension.data()} == name;
                });
            }};

            const auto &properties{propertiesChain.get<vk::PhysicalDeviceProperties2>().properties};
            const auto &driver{propertiesChain.get<vk::PhysicalDeviceDriverProperties>()};
            const auto memory{physicalDevice.getMemoryProperties()};
            bool budgetSupported{supported("VK_EXT_memory_budget")};
            bool budgetAvailable{};
            std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> budgets{}, usages{};
            std::string_view budgetStatus{budgetSupported ? "query_unavailable" : "unsupported"};
#ifdef VK_EXT_memory_budget
            if (budgetSupported && physicalDevice.getDispatcher()->vkGetPhysicalDeviceMemoryProperties2) {
                VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
                budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
                VkPhysicalDeviceMemoryProperties2 queriedMemory{};
                queriedMemory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
                queriedMemory.pNext = &budget;
                physicalDevice.getDispatcher()->vkGetPhysicalDeviceMemoryProperties2(static_cast<VkPhysicalDevice>(*physicalDevice), &queriedMemory);
                std::copy_n(budget.heapBudget, VK_MAX_MEMORY_HEAPS, budgets.begin());
                std::copy_n(budget.heapUsage, VK_MAX_MEMORY_HEAPS, usages.begin());
                budgetAvailable = true;
                budgetStatus = "available";
            }
#else
            if (budgetSupported)
                budgetStatus = "headers_unavailable";
#endif

            std::ostringstream json;
            json.imbue(std::locale::classic());
            json << std::boolalpha;
            json << "{\n\"schema_version\":1,\"snapshot_stage\":\"logical_device_created\","
                    "\"memory_units\":\"bytes\",\"periodic_sampling\":false,";
            json << "\n\"device\":{\"name\":" << quote(properties.deviceName.data())
                 << ",\"type\":" << quote(vk::to_string(properties.deviceType))
                 << ",\"vendor_id\":" << properties.vendorID << ",\"device_id\":" << properties.deviceID
                 << ",\"api_version_raw\":" << properties.apiVersion
                 << ",\"api_version\":" << quote(std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) + "." + std::to_string(VK_API_VERSION_MINOR(properties.apiVersion)) + "." + std::to_string(VK_API_VERSION_PATCH(properties.apiVersion)))
                 << ",\"queue_family_index\":" << queueFamilyIndex << "},";
            // Driver versions use vendor-specific encodings; retain the raw value.
            json << "\n\"driver\":{\"id\":" << quote(vk::to_string(driver.driverID))
                 << ",\"name\":" << quote(driver.driverName.data())
                 << ",\"info\":" << quote(driver.driverInfo.data())
                 << ",\"version_raw\":" << properties.driverVersion << "},";

            json << "\n\"android\":{";
#ifdef __ANDROID__
            constexpr const char *AndroidProperties[]{"ro.product.manufacturer", "ro.product.model", "ro.hardware", "ro.soc.manufacturer", "ro.soc.model", "ro.build.version.release", "ro.build.version.sdk"};
            bool firstProperty{true};
            for (const auto property : AndroidProperties) {
                char value[PROP_VALUE_MAX]{};
                if (__system_property_get(property, value) <= 0)
                    continue;
                if (!firstProperty)
                    json << ',';
                firstProperty = false;
                json << quote(property) << ':' << quote(value);
            }
#endif
            json << "},\n\"device_extensions_supported\":[";
            for (size_t index{}; index < supportedExtensions.size(); index++) {
                if (index)
                    json << ',';
                const auto &extension{supportedExtensions[index]};
                json << "{\"name\":" << quote(extension.extensionName.data()) << ",\"spec_version\":" << extension.specVersion << '}';
            }
            json << "],\n\"device_extensions_enabled\":[";
            for (size_t index{}; index < enabledExtensions.size(); index++) {
                if (index)
                    json << ',';
                json << quote(enabledExtensions[index].data());
            }
            json << "],\n\"traits\":{\"summary\":" << quote(traits.Summary())
                 << ",\"supports_null_descriptor\":" << traits.supportsNullDescriptor
                 << ",\"supports_image_read_without_format\":" << traits.supportsImageReadWithoutFormat
                 << ",\"supports_image_write_without_format\":" << traits.supportsShaderStorageImageWriteWithoutFormat
                 << ",\"supports_adreno_direct_memory_import\":" << traits.supportsAdrenoDirectMemoryImport << "},";
            const auto &quirks{traits.quirks};
            json << "\n\"quirks\":{\"summary\":" << quote(traits.quirks.Summary())
                 << ",\"needs_individual_texture_binding_writes\":" << quirks.needsIndividualTextureBindingWrites
                 << ",\"image_mutable_format_costly\":" << quirks.vkImageMutableFormatCostly
                 << ",\"adreno_relaxed_format_aliasing\":" << quirks.adrenoRelaxedFormatAliasing
                 << ",\"adreno_broken_format_report\":" << quirks.adrenoBrokenFormatReport
                 << ",\"relaxed_render_pass_compatibility\":" << quirks.relaxedRenderPassCompatibility
                 << ",\"broken_push_descriptors\":" << quirks.brokenPushDescriptors
                 << ",\"broken_spirv_position_input\":" << quirks.brokenSpirvPositionInput
                 << ",\"broken_spirv_access_chain_opt\":" << quirks.brokenSpirvAccessChainOpt
                 << ",\"broken_compute_shaders\":" << quirks.brokenComputeShaders
                 << ",\"broken_multithreaded_pipeline_compilation\":" << quirks.brokenMultithreadedPipelineCompilation
                 << ",\"broken_subgroup_mask_extract_dynamic\":" << quirks.brokenSubgroupMaskExtractDynamic
                 << ",\"broken_subgroup_shuffle\":" << quirks.brokenSubgroupShuffle
                 << ",\"broken_spirv_vector_access_chain\":" << quirks.brokenSpirvVectorAccessChain
                 << ",\"broken_dynamic_state_vertex_bindings\":" << quirks.brokenDynamicStateVertexBindings
                 << ",\"max_subpass_count\":" << quirks.maxSubpassCount
                 << ",\"max_global_priority\":" << quote(vk::to_string(quirks.maxGlobalPriority)) << "},";

            json << "\n\"memory_budget\":{\"supported\":" << budgetSupported
                 << ",\"enabled\":" << enabled("VK_EXT_memory_budget")
                 << ",\"available\":" << budgetAvailable << ",\"status\":" << quote(budgetStatus)
                 << ",\"scope\":\"driver_heap_estimates\",\"process_gpu_allocated_bytes\":null},";
            json << "\n\"memory_heaps\":[";
            for (u32 index{}; index < memory.memoryHeapCount; index++) {
                if (index)
                    json << ',';
                const auto &heap{memory.memoryHeaps[index]};
                json << "{\"index\":" << index << ",\"size_bytes\":" << heap.size
                     << ",\"flags_raw\":" << static_cast<VkMemoryHeapFlags>(heap.flags)
                     << ",\"flags\":" << quote(vk::to_string(heap.flags)) << ",\"budget_bytes\":";
                if (budgetAvailable)
                    json << budgets[index];
                else
                    json << "null";
                json << ",\"usage_bytes\":";
                if (budgetAvailable)
                    json << usages[index];
                else
                    json << "null";
                json << '}';
            }
            json << "],\n\"memory_types\":[";
            for (u32 index{}; index < memory.memoryTypeCount; index++) {
                if (index)
                    json << ',';
                const auto &type{memory.memoryTypes[index]};
                json << "{\"index\":" << index << ",\"heap_index\":" << type.heapIndex
                     << ",\"property_flags_raw\":" << static_cast<VkMemoryPropertyFlags>(type.propertyFlags)
                     << ",\"property_flags\":" << quote(vk::to_string(type.propertyFlags)) << '}';
            }
            json << "]\n}\n";
            skyline::diagnostics::WriteJson("vulkan.json", json.str());
        } catch (...) {
            skyline::diagnostics::WriteJson("vulkan.json", "{\"schema_version\":1,\"status\":\"snapshot_failed\"}\n");
        }
    }
}
