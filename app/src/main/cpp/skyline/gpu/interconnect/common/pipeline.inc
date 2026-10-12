// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <gpu/interconnect/command_executor.h>
#include "samplers.h"
#include "textures.h"

namespace skyline::gpu::interconnect {
    union BindlessHandle {
        u32 raw;

        struct {
            u32 textureIndex : 20;
            u32 samplerIndex : 12;
        };
    };

    static DynamicBufferBinding GetConstantBufferBinding(InterconnectContext &ctx,
                                                         span<const u32, Shader::Info::MAX_CBUFS> cbufSizes,
                                                         BufferView view, size_t idx,
                                                         vk::PipelineStageFlagBits dstStage,
                                                         vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        if (!view) // Return a dummy buffer if the constant buffer isn't bound
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        size_t sizeOverride{std::min<size_t>(cbufSizes[idx], view.size)};
        if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag, sizeOverride)}) {
            return megaBufferBinding;
        } else {
            view.GetBuffer()->BlockSequencedCpuBackingWrites();
            return view;
        }
    }

    static DynamicBufferBinding GetStorageBufferBinding(InterconnectContext &ctx, const auto &desc,
                                                        ConstantBuffer &cbuf, CachedMappedBufferView &cachedView,
                                                        vk::PipelineStageFlagBits dstStage,
                                                        vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        struct SsboDescriptor {
            u64 address;
            u32 size;
        };
        auto ssbo{cbuf.Read<SsboDescriptor>(ctx.executor, desc.cbuf_offset)};
        if (ssbo.size == 0)
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        size_t padding{ssbo.address & (ctx.gpu.traits.minimumStorageBufferAlignment - 1)};
        cachedView.Update(ctx, ssbo.address - padding, util::AlignUp(ssbo.size + padding, ctx.gpu.traits.minimumStorageBufferAlignment));
        if (!cachedView.view) // Return a dummy buffer if the SSBO isn't bound
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        auto view{cachedView.view};
        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        if (desc.is_written) {
            if (view.GetBuffer()->SequencedCpuBackingWritesBlocked()) {
                srcStageMask |= vk::PipelineStageFlagBits::eAllCommands;
                dstStageMask |= dstStage;
            }

            view.GetBuffer()->MarkGpuDirty(ctx.executor.usageTracker);
        } else {
            if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag)})
                return megaBufferBinding;
        }

        view.GetBuffer()->BlockSequencedCpuBackingWrites();

        return view;
    }

    static BindlessHandle ReadBindlessHandle(InterconnectContext &ctx, auto &constantBuffers, const auto &desc, size_t arrayIdx) {
        ConstantBuffer &primaryCbuf{constantBuffers[desc.cbuf_index]};
        if (!primaryCbuf.view)
            return { .raw = 0 };

        size_t elemOffset{arrayIdx << desc.size_shift};
        size_t primaryCbufOffset{desc.cbuf_offset + elemOffset};
        u32 primaryVal{primaryCbuf.Read<u32>(ctx.executor, primaryCbufOffset)};

        if constexpr (requires { desc.has_secondary; }) {
            if (desc.has_secondary) {
                ConstantBuffer &secondaryCbuf{constantBuffers[desc.secondary_cbuf_index]};
                if (!secondaryCbuf.view)
                    return {.raw = 0};
                size_t secondaryCbufOffset{desc.secondary_cbuf_offset + elemOffset};
                u32 secondaryVal{secondaryCbuf.Read<u32>(ctx.executor, secondaryCbufOffset)};
                return {.raw = (primaryVal << desc.shift_left) | (secondaryVal << desc.secondary_shift_left)};
            }
        }

        if constexpr (requires { desc.shift_left; })
            return {.raw = primaryVal << desc.shift_left};
        else
            return {.raw = primaryVal};
    }

    static TexelBufferDescriptors AllocateTexelBufferDescriptors(InterconnectContext &ctx, size_t count) {
        if (!count)
            return {};
        auto lifetime{std::make_shared<std::vector<vk::raii::BufferView>>()};
        lifetime->reserve(count);
        ctx.executor.AttachDependency(lifetime);
        return {
            ctx.executor.allocator->AllocateUntracked<TexelBufferBinding>(count),
            ctx.executor.allocator->AllocateUntracked<vk::BufferView>(count),
            lifetime.get(),
        };
    }

    static TexelBufferBinding GetTexelBufferBinding(InterconnectContext &ctx, const auto &desc,
                                                   Textures &textures, BindlessHandle handle,
                                                   vk::PipelineStageFlagBits dstStage,
                                                   vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        constexpr bool storage{requires { desc.is_written; }};
        Shader::ImageFormat format{Shader::ImageFormat::Typeless};
        if constexpr (storage)
            format = desc.format;
        auto binding{textures.GetTexelBuffer(ctx, handle.textureIndex, storage, format)};
        auto *buffer{binding.view.GetBuffer()};
        buffer->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);
        if constexpr (storage) {
            if (desc.is_written) {
                if (buffer->SequencedCpuBackingWritesBlocked()) {
                    srcStageMask |= vk::PipelineStageFlagBits::eAllCommands;
                    dstStageMask |= dstStage;
                }
                buffer->MarkGpuDirty(ctx.executor.usageTracker);
            }
        }
        buffer->BlockSequencedCpuBackingWrites();
        return binding;
    }

    static std::pair<vk::DescriptorImageInfo, TextureView *> GetTextureBinding(InterconnectContext &ctx, const auto &desc,
                                                                               Samplers &samplers, Textures &textures,
                                                                               BindlessHandle handle,
                                                                               vk::PipelineStageFlagBits dstStage,
                                                                               vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto sampler{samplers.GetSampler(ctx, handle.samplerIndex, handle.textureIndex)};
        auto texture{textures.GetTexture(ctx, handle.textureIndex, desc.type)};
        ctx.executor.AttachTexture(texture);
        auto view{texture->GetView()};
        texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        return {
            vk::DescriptorImageInfo{
                .sampler = **sampler,
                .imageView = view,
                .imageLayout = texture->texture->layout
            },
            texture
        };
    }

    static std::pair<vk::DescriptorImageInfo, TextureView *> GetStorageImageBinding(InterconnectContext &ctx, const auto &desc,
                                                                                     Textures &textures, BindlessHandle handle,
                                                                                     vk::PipelineStageFlagBits dstStage,
                                                                                     vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto texture{textures.GetTexture(ctx, handle.textureIndex, desc.type)};
        ctx.executor.AttachTexture(texture);

        texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);
        texture->texture->TransitionLayout(vk::ImageLayout::eGeneral);
        if (desc.is_written)
            texture->texture->MarkGpuDirty(ctx.executor.usageTracker);

        return {
            vk::DescriptorImageInfo{
                .sampler = nullptr,
                .imageView = texture->GetView(),
                .imageLayout = vk::ImageLayout::eGeneral,
            },
            texture
        };
    }

}
