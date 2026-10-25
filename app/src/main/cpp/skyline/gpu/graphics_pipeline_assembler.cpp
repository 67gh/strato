// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <boost/functional/hash.hpp>
#include <common/diagnostic_recorder.h>
#include <system_error>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <gpu.h>
#include "graphics_pipeline_assembler.h"
#include "trait_manager.h"

namespace skyline::gpu {
    /**
     * @brief Unique header serialized into the pipeline cache filename as a hexdump to identify a particular driver
     */
    struct PipelineCacheFileNameHeader {
        u32 vendorId; //!< The driver reported vendor ID
        u32 deviceId; //!< The driver reported device ID
        u32 driverVersion; //!< The driver reported version
        std::array<u8, VK_UUID_SIZE> uuid; //!< The driver reported pipeline cache UUID

        PipelineCacheFileNameHeader(const TraitManager &traits)
            : vendorId{traits.vendorId},
              deviceId{traits.deviceId},
              driverVersion{traits.driverVersion},
              uuid{traits.pipelineCacheUuid} {}

        std::string HexDump() {
            return util::HexDump(span<u8>{reinterpret_cast<u8 *>(this), sizeof(PipelineCacheFileNameHeader)});
        }
    };

    /**
     * @brief Header that precedes serialized pipeline cache data in the pipeline cache file
     */
    struct PipelineCacheFileDataHeader {
        u64 size;
        u64 hash;
        u8 data[];
    };
    static_assert(sizeof(PipelineCacheFileDataHeader) == 0x10);

    // Disk caches are optional. Never allocate an unbounded size from a file header.
    static constexpr u64 MaxPipelineCacheBytes{128ULL * 1024 * 1024};

    static vk::raii::PipelineCache DeserialisePipelineCache(GPU &gpu, std::string_view pipelineCacheDir) {
        try {
            std::filesystem::create_directories(pipelineCacheDir);
            PipelineCacheFileNameHeader expectedFilenameHeader{gpu.traits};
            auto path{std::filesystem::path{pipelineCacheDir} / expectedFilenameHeader.HexDump()};
            if (!std::filesystem::exists(path))
                return {gpu.vkDevice, vk::PipelineCacheCreateInfo{}};

            std::ifstream stream{path, std::ios::binary | std::ios::ate};
            const auto length{stream.tellg()};
            if (!stream || length < static_cast<std::streamoff>(sizeof(PipelineCacheFileDataHeader)))
                throw exception("Truncated Vulkan pipeline cache header");
            stream.seekg(0);
            PipelineCacheFileDataHeader header{};
            if (!stream.read(reinterpret_cast<char *>(&header), sizeof(header)))
                throw exception("Failed to read Vulkan pipeline cache header");
            const auto payloadSize{static_cast<u64>(length) - sizeof(header)};
            if (header.size != payloadSize || header.size > MaxPipelineCacheBytes ||
                header.size > static_cast<u64>(std::numeric_limits<std::streamsize>::max()))
                throw exception("Invalid Vulkan pipeline cache size");

            std::vector<u8> readData(static_cast<size_t>(header.size));
            if (header.size && !stream.read(reinterpret_cast<char *>(readData.data()), static_cast<std::streamsize>(header.size)))
                throw exception("Truncated Vulkan pipeline cache payload");
            if (header.hash != XXH64(readData.data(), readData.size(), 0))
                throw exception("Invalid Vulkan pipeline cache hash");
            return {gpu.vkDevice, vk::PipelineCacheCreateInfo{
                .initialDataSize = readData.size(),
                .pInitialData = readData.data(),
            }};
        } catch (const std::exception &error) {
            LOGW("Ignoring unusable Vulkan pipeline cache: {}", error.what());
        }
        // An error creating the empty cache must still propagate.
        return {gpu.vkDevice, vk::PipelineCacheCreateInfo{}};
    }

    static void SerialisePipelineCache(GPU &gpu, std::string_view pipelineCacheDir, span<u8> data) {
        if (data.size() > MaxPipelineCacheBytes) {
            LOGW("Skipping oversized Vulkan pipeline cache: {} bytes", data.size());
            return;
        }
        std::filesystem::create_directories(pipelineCacheDir);
        PipelineCacheFileNameHeader expectedFilenameHeader{gpu.traits};
        auto path{std::filesystem::path{pipelineCacheDir} / expectedFilenameHeader.HexDump()};
        std::string temporary{path.string() + ".tmp.XXXXXX"};
        int fd{mkstemp(temporary.data())};
        if (fd < 0) {
            LOGW("Cannot create Vulkan cache temporary file (errno={})", errno);
            return;
        }
        auto cleanup{[&](int *) noexcept {
            if (fd >= 0) close(fd);
            unlink(temporary.c_str());
        }};
        std::unique_ptr<int, decltype(cleanup)> guard{&fd, cleanup};
        auto writeAll{[&](const void *data, size_t size) {
            auto bytes{static_cast<const u8 *>(data)};
            while (size) {
                auto written{write(fd, bytes, size)};
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) return false;
                bytes += written;
                size -= static_cast<size_t>(written);
            }
            return true;
        }};
        PipelineCacheFileDataHeader header{.size = data.size(), .hash = XXH64(data.data(), data.size(), 0)};
        if (!writeAll(&header, sizeof(header)) || !writeAll(data.data(), data.size()) || fsync(fd) != 0) {
            LOGW("Vulkan pipeline cache write failed (errno={})", errno);
            return;
        }
        const int closeResult{close(fd)};
        fd = -1; // Never retry close: the descriptor may have been reused.
        if (closeResult != 0 || std::rename(temporary.c_str(), path.c_str()) != 0) {
            LOGW("Vulkan pipeline cache replacement failed (errno={})", errno);
            return;
        }
        LOGI("Wrote Vulkan pipeline cache to {} (size: 0x{:X} bytes)", path.string(), data.size());
    }

    GraphicsPipelineAssembler::GraphicsPipelineAssembler(GPU &gpu, std::string_view pipelineCacheDir)
        : gpu{gpu},
          vkPipelineCache{DeserialisePipelineCache(gpu, pipelineCacheDir)},
          pool{gpu.traits.quirks.brokenMultithreadedPipelineCompilation ? 1U : 0U},
          pipelineCacheDir{pipelineCacheDir} {}

    GraphicsPipelineAssembler::~GraphicsPipelineAssembler() {
        // Drain workers before members used by those workers are destroyed.
        pool.wait_for_tasks();
    }

    void GraphicsPipelineAssembler::ReleasePipelineDescription(std::list<PipelineDescription>::iterator it) {
        if (it->destroyShaderModules)
            for (const auto &stage : it->shaderStages)
                (*gpu.vkDevice).destroyShaderModule(stage.module, nullptr, *gpu.vkDevice.getDispatcher());
        std::scoped_lock lock{mutex};
        compilePendingDescs.erase(it);
    }

    #define VEC_CPY(pointer, size) state.pointer, state.pointer + state.size

    GraphicsPipelineAssembler::PipelineDescription::PipelineDescription(const GraphicsPipelineAssembler::PipelineState &state)
        : shaderStages(state.shaderStages.begin(), state.shaderStages.end()),
          vertexState(state.vertexState),
          vertexBindings(VEC_CPY(VertexInputState().pVertexBindingDescriptions, VertexInputState().vertexBindingDescriptionCount)),
          vertexAttributes(VEC_CPY(VertexInputState().pVertexAttributeDescriptions, VertexInputState().vertexAttributeDescriptionCount)),
          vertexDivisors(VEC_CPY(VertexDivisorState().pVertexBindingDivisors, VertexDivisorState().vertexBindingDivisorCount)),
          inputAssemblyState(state.inputAssemblyState),
          tessellationState(state.tessellationState),
          viewportState(state.viewportState),
          viewports(VEC_CPY(viewportState.pViewports, viewportState.viewportCount)),
          scissors(VEC_CPY(viewportState.pScissors, viewportState.scissorCount)),
          rasterizationState(state.rasterizationState),
          multisampleState(state.multisampleState),
          depthStencilState(state.depthStencilState),
          colorBlendState(state.colorBlendState),
          dynamicStates(VEC_CPY(dynamicState.pDynamicStates, dynamicState.dynamicStateCount)),
          dynamicState(state.dynamicState),
          colorBlendAttachments(VEC_CPY(colorBlendState.pAttachments, colorBlendState.attachmentCount)) {
        auto &vertexInputState{vertexState.get<vk::PipelineVertexInputStateCreateInfo>()};
        vertexInputState.pVertexBindingDescriptions = vertexBindings.data();
        vertexInputState.pVertexAttributeDescriptions = vertexAttributes.data();
        vertexState.get<vk::PipelineVertexInputDivisorStateCreateInfoEXT>().pVertexBindingDivisors = vertexDivisors.data();

        viewportState.pViewports = viewports.data();
        viewportState.pScissors = scissors.data();

        colorBlendState.pAttachments = colorBlendAttachments.data();

        dynamicState.pDynamicStates = dynamicStates.data();

        for (auto &colorFormat : state.colorFormats)
            colorFormats.emplace_back(colorFormat);

        depthStencilFormat = state.depthStencilFormat;
        sampleCount = state.sampleCount;
        destroyShaderModules = state.destroyShaderModules;
    }

    #undef VEC_CPY

    vk::raii::Pipeline GraphicsPipelineAssembler::AssemblePipeline(std::list<PipelineDescription>::iterator pipelineDescIt, vk::PipelineLayout pipelineLayout) {
        auto release{[this, pipelineDescIt](PipelineDescription *) { ReleasePipelineDescription(pipelineDescIt); }};
        std::unique_ptr<PipelineDescription, decltype(release)> descriptionGuard{&*pipelineDescIt, release};
        boost::container::small_vector<vk::AttachmentDescription, 8> attachmentDescriptions;
        boost::container::small_vector<vk::AttachmentReference, 8> attachmentReferences;

        auto pushAttachment{[&](vk::Format format) {
            if (format != vk::Format::eUndefined) {
                attachmentDescriptions.push_back(vk::AttachmentDescription{
                    .format = format,
                    .samples = pipelineDescIt->sampleCount,
                    .loadOp = vk::AttachmentLoadOp::eLoad,
                    .storeOp = vk::AttachmentStoreOp::eStore,
                    .stencilLoadOp = vk::AttachmentLoadOp::eLoad,
                    .stencilStoreOp = vk::AttachmentStoreOp::eStore,
                    .initialLayout = vk::ImageLayout::eGeneral,
                    .finalLayout = vk::ImageLayout::eGeneral,
                    .flags = vk::AttachmentDescriptionFlagBits::eMayAlias
                });
                attachmentReferences.push_back(vk::AttachmentReference{
                    .attachment = static_cast<u32>(attachmentDescriptions.size() - 1),
                    .layout = vk::ImageLayout::eGeneral,
                });
            } else {
                attachmentReferences.push_back(vk::AttachmentReference{
                    .attachment = VK_ATTACHMENT_UNUSED,
                    .layout = vk::ImageLayout::eUndefined,
                });
            }
        }};

        vk::SubpassDescription subpassDescription{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        };

        for (auto &colorAttachment : pipelineDescIt->colorFormats)
            pushAttachment(colorAttachment);

        if (pipelineDescIt->depthStencilFormat != vk::Format::eUndefined) {
            pushAttachment(pipelineDescIt->depthStencilFormat);

            subpassDescription.pColorAttachments = attachmentReferences.data();
            subpassDescription.colorAttachmentCount = static_cast<u32>(attachmentReferences.size() - 1);
            subpassDescription.pDepthStencilAttachment = &attachmentReferences.back();
        } else {
            subpassDescription.pColorAttachments = attachmentReferences.data();
            subpassDescription.colorAttachmentCount = static_cast<u32>(attachmentReferences.size());
        }

        vk::raii::RenderPass renderPass{gpu.vkDevice, vk::RenderPassCreateInfo{
            .attachmentCount = static_cast<u32>(attachmentDescriptions.size()),
            .pAttachments = attachmentDescriptions.data(),
            .subpassCount = 1,
            .pSubpasses = &subpassDescription,
        }};

        const auto diagnosticId=skyline::diagnostics::NewId();
        skyline::diagnostics::Operation diagnosticOperation("pipeline_compile", diagnosticId, skyline::diagnostics::Timing::Pipeline);
        auto pipeline{[&] {
            try {
                return gpu.vkDevice.createGraphicsPipeline(vkPipelineCache, vk::GraphicsPipelineCreateInfo{
                    .pStages = pipelineDescIt->shaderStages.data(),
                    .stageCount = static_cast<u32>(pipelineDescIt->shaderStages.size()),
                    .pVertexInputState = &pipelineDescIt->vertexState.get<vk::PipelineVertexInputStateCreateInfo>(),
                    .pInputAssemblyState = &pipelineDescIt->inputAssemblyState,
                    .pTessellationState = &pipelineDescIt->tessellationState,
                    .pViewportState = &pipelineDescIt->viewportState,
                    .pRasterizationState = &pipelineDescIt->rasterizationState.get<vk::PipelineRasterizationStateCreateInfo>(),
                    .pMultisampleState = &pipelineDescIt->multisampleState,
                    .pDepthStencilState = &pipelineDescIt->depthStencilState,
                    .pColorBlendState = &pipelineDescIt->colorBlendState,
                    .pDynamicState = &pipelineDescIt->dynamicState,
                    .layout = pipelineLayout,
                    .renderPass = *renderPass,
                    .subpass = 0,
                });
            } catch (const std::system_error &error) {
                skyline::diagnostics::Event("pipeline_create_failed", diagnosticId, static_cast<std::uint64_t>(error.code().value()), 0, error.what());
                throw;
            } catch (const std::exception &error) {
                skyline::diagnostics::Event("pipeline_create_failed", diagnosticId, 0, 0, error.what());
                throw;
            }
        }()};

        diagnosticOperation.Finish(0,"graphics_pipeline_created");
        descriptionGuard.reset();
        std::function<void()> callback;
        {
            std::scoped_lock lock{mutex};
            callback = compilationCallback;
        }
        if (callback)
            callback();

        return pipeline;
    }


    GraphicsPipelineAssembler::CompiledPipeline GraphicsPipelineAssembler::AssemblePipelineAsync(const PipelineState &state, span<const vk::DescriptorSetLayoutBinding> layoutBindings, span<const vk::PushConstantRange> pushConstantRanges, bool noPushDescriptors) {
        // Ownership of temporary shader modules transfers only once the description exists.
        auto releaseInput{[this](const PipelineState *input) {
            if (input->destroyShaderModules)
                for (const auto &stage : input->shaderStages)
                    (*gpu.vkDevice).destroyShaderModule(stage.module, nullptr, *gpu.vkDevice.getDispatcher());
        }};
        std::unique_ptr<const PipelineState, decltype(releaseInput)> inputGuard{&state, releaseInput};
        vk::raii::DescriptorSetLayout descriptorSetLayout{gpu.vkDevice, vk::DescriptorSetLayoutCreateInfo{
            .flags = vk::DescriptorSetLayoutCreateFlags{(!noPushDescriptors && gpu.traits.supportsPushDescriptors) ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{}},
            .pBindings = layoutBindings.data(),
            .bindingCount = static_cast<u32>(layoutBindings.size()),
        }};

        vk::raii::PipelineLayout pipelineLayout{gpu.vkDevice, vk::PipelineLayoutCreateInfo{
            .pSetLayouts = &*descriptorSetLayout,
            .setLayoutCount = 1,
            .pPushConstantRanges = pushConstantRanges.data(),
            .pushConstantRangeCount = static_cast<u32>(pushConstantRanges.size()),
        }};

        auto descIt{[this, &state]() {
            std::scoped_lock lock{mutex};
            compilePendingDescs.emplace_back(state);
            return std::prev(compilePendingDescs.end());
        }()};

        inputGuard.release();
        auto release{[this, descIt](PipelineDescription *) { ReleasePipelineDescription(descIt); }};
        std::unique_ptr<PipelineDescription, decltype(release)> submitGuard{&*descIt, release};
        auto pipelineFuture{pool.submit(&GraphicsPipelineAssembler::AssemblePipeline, this, descIt, *pipelineLayout)};
        submitGuard.release(); // The worker now owns cleanup, including exceptional exits.
        return CompiledPipeline{std::move(descriptorSetLayout), std::move(pipelineLayout), std::move(pipelineFuture)};
    }

    void GraphicsPipelineAssembler::WaitIdle() {
        pool.wait_for_tasks();
    }

    void GraphicsPipelineAssembler::SavePipelineCache() {
        std::ignore = pool.submit([this] () {
            try {
                std::vector<u8> rawData{vkPipelineCache.getData()};
                SerialisePipelineCache(gpu, pipelineCacheDir, rawData);
            } catch (const std::exception &error) {
                LOGW("Cannot save Vulkan pipeline cache: {}", error.what());
            }
        });
    }

    void GraphicsPipelineAssembler::RegisterCompilationCallback(std::function<void()> callback) {
        std::scoped_lock lock{mutex};
        if (compilationCallback)
            throw exception("A compilation callback is already registered");

        compilationCallback = std::move(callback);
    }

    void GraphicsPipelineAssembler::UnregisterCompilationCallback() {
        std::scoped_lock lock{mutex};
        compilationCallback = {};
    }
}
