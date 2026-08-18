// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <vfs/filesystem.h>
#include <vfs/backing.h>
#include "frame_generator.h"

namespace skyline::gpu {
    namespace {
        vk::raii::ShaderModule CreateShaderModule(GPU &gpu, vfs::Backing &shaderBacking) {
            std::vector<u32> shaderBuf(shaderBacking.size / 4);

            if (shaderBacking.Read(span(shaderBuf)) != shaderBacking.size)
                throw exception("FrameGenerator: Failed to read shader");

            return gpu.vkDevice.createShaderModule({
                .pCode = shaderBuf.data(),
                .codeSize = shaderBacking.size,
            });
        }

        vk::raii::DescriptorSetLayout CreateSetLayout(GPU &gpu, span<const vk::DescriptorSetLayoutBinding> bindings) {
            return gpu.vkDevice.createDescriptorSetLayout({
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            });
        }
    }

    struct MotionEstimatePushConstants {
        i32 frameExtentX, frameExtentY;
        i32 motionOutExtentX, motionOutExtentY;
        i32 blockSize;
        i32 searchRadius;
        i32 hasPrevLevel;
    };

    struct InterpolatePushConstants {
        i32 frameExtentX, frameExtentY;
        i32 blockSize;
        float t;
    };

    FrameGenerator::FrameGenerator(GPU &pGpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem)
        : gpu{pGpu},
          motionEstimateShaderModule{CreateShaderModule(gpu, *shaderFileSystem->OpenFile("shaders/fg_motion_estimate.comp.spv"))},
          frameInterpolateShaderModule{CreateShaderModule(gpu, *shaderFileSystem->OpenFile("shaders/fg_frame_interpolate.comp.spv"))},
          motionEstimateSetLayout{CreateSetLayout(gpu, std::array<vk::DescriptorSetLayoutBinding, 4>{{
              {.binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
              {.binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
              {.binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
              {.binding = 3, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
          }})},
          frameInterpolateSetLayout{CreateSetLayout(gpu, std::array<vk::DescriptorSetLayoutBinding, 4>{{
              {.binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
              {.binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
              {.binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
              {.binding = 3, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
          }})},
          motionEstimatePipelineLayout{gpu.vkDevice.createPipelineLayout({
              .setLayoutCount = 1,
              .pSetLayouts = &*motionEstimateSetLayout,
              .pushConstantRangeCount = 1,
              .pPushConstantRanges = std::array{vk::PushConstantRange{vk::ShaderStageFlagBits::eCompute, 0, sizeof(MotionEstimatePushConstants)}}.data(),
          })},
          frameInterpolatePipelineLayout{gpu.vkDevice.createPipelineLayout({
              .setLayoutCount = 1,
              .pSetLayouts = &*frameInterpolateSetLayout,
              .pushConstantRangeCount = 1,
              .pPushConstantRanges = std::array{vk::PushConstantRange{vk::ShaderStageFlagBits::eCompute, 0, sizeof(InterpolatePushConstants)}}.data(),
          })},
          motionEstimatePipeline{gpu.vkDevice.createComputePipeline(nullptr, {
              .stage = {
                  .stage = vk::ShaderStageFlagBits::eCompute,
                  .module = *motionEstimateShaderModule,
                  .pName = "main",
              },
              .layout = *motionEstimatePipelineLayout,
          })},
          frameInterpolatePipeline{gpu.vkDevice.createComputePipeline(nullptr, {
              .stage = {
                  .stage = vk::ShaderStageFlagBits::eCompute,
                  .module = *frameInterpolateShaderModule,
                  .pName = "main",
              },
              .layout = *frameInterpolatePipelineLayout,
          })},
          linearSampler{gpu.vkDevice.createSampler({
              .magFilter = vk::Filter::eLinear,
              .minFilter = vk::Filter::eLinear,
              .mipmapMode = vk::SamplerMipmapMode::eNearest,
              .addressModeU = vk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vk::SamplerAddressMode::eClampToEdge,
              .addressModeW = vk::SamplerAddressMode::eClampToEdge,
          })} {}

    namespace {
        std::shared_ptr<Texture> CreateStorageTexture(GPU &gpu, texture::Dimensions extent, texture::Format format, vk::ImageUsageFlags usage) {
            auto vkImage{gpu.memory.AllocateImage({
                .imageType = vk::ImageType::e2D,
                .format = format->vkFormat,
                .extent = extent,
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = usage,
                .sharingMode = vk::SharingMode::eExclusive,
                .queueFamilyIndexCount = 1,
                .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
                .initialLayout = vk::ImageLayout::eUndefined,
            })};

            auto texture{std::make_shared<Texture>(gpu, std::move(vkImage), extent, format, vk::ImageLayout::eUndefined, vk::ImageTiling::eOptimal, vk::ImageCreateFlags{}, usage)};
            texture->TransitionLayout(vk::ImageLayout::eGeneral);
            return texture;
        }
    }

    void FrameGenerator::EnsureResources(texture::Dimensions extent) {
        if (extent == cachedExtent && motionFieldFine)
            return;
        cachedExtent = extent;

        constexpr texture::Format MotionFormat{format::R16G16Sfloat};
        constexpr texture::Format FrameFormat{format::R8G8B8A8Unorm};

        texture::Dimensions coarseExtent{util::AlignUp(extent.width, BaseBlockSize * 2) / (BaseBlockSize * 2), util::AlignUp(extent.height, BaseBlockSize * 2) / (BaseBlockSize * 2), 1};
        texture::Dimensions fineExtent{util::AlignUp(extent.width, BaseBlockSize) / BaseBlockSize, util::AlignUp(extent.height, BaseBlockSize) / BaseBlockSize, 1};

        auto motionUsage{vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled};
        motionFieldCoarse = CreateStorageTexture(gpu, coarseExtent, MotionFormat, motionUsage);
        motionFieldFine = CreateStorageTexture(gpu, fineExtent, MotionFormat, motionUsage);

        auto frameUsage{vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc};
        for (auto &frame : generatedFrames)
            frame = CreateStorageTexture(gpu, extent, FrameFormat, frameUsage);
    }

    void FrameGenerator::DispatchMotionEstimate(vk::raii::CommandBuffer &cmd, TextureView *prevFrame, TextureView *currFrame,
                                                 Texture *motionOut, Texture *prevLevelMotion, texture::Dimensions levelExtent, int blockSize) {
        auto set{gpu.descriptor.AllocateSet(*motionEstimateSetLayout)};

        auto motionOutView{motionOut->GetView(vk::ImageViewType::e2D, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})};
        // When there's no coarser level yet (topmost pyramid level) we still need a bound image; reuse motionOut itself,
        // the shader is instructed via 'hasPrevLevel' not to actually read from it in that case.
        auto prevLevelView{(prevLevelMotion ? prevLevelMotion : motionOut)->GetView(vk::ImageViewType::e2D, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})};

        std::array<vk::DescriptorImageInfo, 4> imageInfos{{
            {*linearSampler, prevFrame->GetView(), vk::ImageLayout::eGeneral},
            {*linearSampler, currFrame->GetView(), vk::ImageLayout::eGeneral},
            {*linearSampler, prevLevelView->GetView(), vk::ImageLayout::eGeneral},
            {nullptr, motionOutView->GetView(), vk::ImageLayout::eGeneral},
        }};

        std::array<vk::WriteDescriptorSet, 4> writes{{
            {*set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos[0]},
            {*set, 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos[1]},
            {*set, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos[2]},
            {*set, 3, 0, 1, vk::DescriptorType::eStorageImage, &imageInfos[3]},
        }};
        gpu.vkDevice.updateDescriptorSets(writes, nullptr);

        texture::Dimensions motionOutExtent{motionOut->dimensions};
        MotionEstimatePushConstants pc{
            .frameExtentX = static_cast<i32>(levelExtent.width), .frameExtentY = static_cast<i32>(levelExtent.height),
            .motionOutExtentX = static_cast<i32>(motionOutExtent.width), .motionOutExtentY = static_cast<i32>(motionOutExtent.height),
            .blockSize = blockSize,
            .searchRadius = SearchRadius,
            .hasPrevLevel = prevLevelMotion ? 1 : 0,
        };

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *motionEstimatePipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *motionEstimatePipelineLayout, 0, *set, nullptr);
        cmd.pushConstants<MotionEstimatePushConstants>(*motionEstimatePipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, pc);
        // One workgroup per output block, the shader itself only uses invocation 0 to perform the search
        cmd.dispatch(motionOutExtent.width, motionOutExtent.height, 1);

        vk::ImageMemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = motionOut->GetBacking(),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader, {}, nullptr, nullptr, barrier);
    }

    void FrameGenerator::DispatchInterpolate(vk::raii::CommandBuffer &cmd, TextureView *prevFrame, TextureView *currFrame,
                                              Texture *motionField, Texture *output, texture::Dimensions extent, float t) {
        auto set{gpu.descriptor.AllocateSet(*frameInterpolateSetLayout)};
        auto outputView{output->GetView(vk::ImageViewType::e2D, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})};
        auto motionView{motionField->GetView(vk::ImageViewType::e2D, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})};

        std::array<vk::DescriptorImageInfo, 4> imageInfos{{
            {*linearSampler, prevFrame->GetView(), vk::ImageLayout::eGeneral},
            {*linearSampler, currFrame->GetView(), vk::ImageLayout::eGeneral},
            {*linearSampler, motionView->GetView(), vk::ImageLayout::eGeneral},
            {nullptr, outputView->GetView(), vk::ImageLayout::eGeneral},
        }};

        std::array<vk::WriteDescriptorSet, 4> writes{{
            {*set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos[0]},
            {*set, 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos[1]},
            {*set, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos[2]},
            {*set, 3, 0, 1, vk::DescriptorType::eStorageImage, &imageInfos[3]},
        }};
        gpu.vkDevice.updateDescriptorSets(writes, nullptr);

        InterpolatePushConstants pc{
            .frameExtentX = static_cast<i32>(extent.width), .frameExtentY = static_cast<i32>(extent.height),
            .blockSize = BaseBlockSize,
            .t = t,
        };

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *frameInterpolatePipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *frameInterpolatePipelineLayout, 0, *set, nullptr);
        cmd.pushConstants<InterpolatePushConstants>(*frameInterpolatePipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, pc);
        cmd.dispatch(util::DivideCeil(extent.width, 8U), util::DivideCeil(extent.height, 8U), 1);
    }

    std::vector<std::shared_ptr<TextureView>> FrameGenerator::GenerateFrames(vk::raii::CommandBuffer &cmd, TextureView *prevFrame, TextureView *currFrame, Mode mode) {
        u32 extraFrames{FrameMultiplier(mode) - 1};
        if (extraFrames == 0)
            return {};

        texture::Dimensions extent{currFrame->texture->dimensions};
        EnsureResources(extent);

        // Two-level pyramid: coarse pass predicts a starting point for the fine pass, this keeps the
        // fine-level search radius (and thus cost) small while still tracking fast motion.
        texture::Dimensions coarseFrameExtent{extent.width / 2, extent.height / 2, 1};
        DispatchMotionEstimate(cmd, prevFrame, currFrame, motionFieldCoarse.get(), nullptr, coarseFrameExtent, BaseBlockSize);
        DispatchMotionEstimate(cmd, prevFrame, currFrame, motionFieldFine.get(), motionFieldCoarse.get(), extent, BaseBlockSize);

        std::vector<std::shared_ptr<TextureView>> result;
        result.reserve(extraFrames);
        for (u32 i{0}; i < extraFrames; i++) {
            float t{static_cast<float>(i + 1) / static_cast<float>(extraFrames + 1)};
            Texture *output{generatedFrames[i].get()};
            DispatchInterpolate(cmd, prevFrame, currFrame, motionFieldFine.get(), output, extent, t);
            result.push_back(output->GetView(vk::ImageViewType::e2D, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}));
        }

        return result;
    }
}
