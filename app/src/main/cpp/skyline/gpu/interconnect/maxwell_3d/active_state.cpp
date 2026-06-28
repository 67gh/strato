// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)
// Copyright © 2022 yuzu Team and Contributors (https://github.com/yuzu-emu/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <limits>
#include <range/v3/algorithm.hpp>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include <gpu/buffer_manager.h>
#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/conversion/quads.h>
#include <gpu/interconnect/common/state_updater.h>
#include "common.h"
#include "active_state.h"

namespace skyline::gpu::interconnect::maxwell3d {
    /* Vertex Buffer */
    void VertexBufferState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, vertexStream.format, vertexStream.location, vertexStreamLimit);
    }

    VertexBufferState::VertexBufferState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine, u32 index) : engine{manager, dirtyHandle, engine}, index{index} {}

    void VertexBufferState::Flush(InterconnectContext &ctx, StateUpdateBuilder &builder, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        size_t size{engine->vertexStreamLimit - engine->vertexStream.location + 1};

        if (engine->vertexStream.format.enable && engine->vertexStream.location != 0 && size) {
            view.Update(ctx, engine->vertexStream.location, size);
            if (*view) {
                ctx.executor.AttachBuffer(*view);
                view->GetBuffer()->PopulateReadBarrier(vk::PipelineStageFlagBits::eVertexInput, srcStageMask, dstStageMask);

                if (megaBufferBinding = view->TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag);
                    megaBufferBinding)
                    builder.SetVertexBuffer(index, megaBufferBinding, ctx.gpu.traits.supportsExtendedDynamicState, engine->vertexStream.format.stride);
                else
                    builder.SetVertexBuffer(index, *view, ctx.gpu.traits.supportsExtendedDynamicState, engine->vertexStream.format.stride);

                return;
            } else {
                LOGW("Unmapped vertex buffer: 0x{:X}", engine->vertexStream.location);
            }
        }

        megaBufferBinding = {};
        if (ctx.gpu.traits.supportsNullDescriptor)
            builder.SetVertexBuffer(index, BufferBinding{}, ctx.gpu.traits.supportsExtendedDynamicState, engine->vertexStream.format.stride);
        else
            builder.SetVertexBuffer(index, {ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, 0).buffer}, ctx.gpu.traits.supportsExtendedDynamicState, engine->vertexStream.format.stride);
    }

    bool VertexBufferState::Refresh(InterconnectContext &ctx, StateUpdateBuilder &builder, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        if (*view)
            view->GetBuffer()->PopulateReadBarrier(vk::PipelineStageFlagBits::eVertexInput, srcStageMask, dstStageMask);

        if (megaBufferBinding) {
            if (auto newMegaBufferBinding{view->TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag)};
                newMegaBufferBinding != megaBufferBinding) {

                megaBufferBinding = newMegaBufferBinding;
                if (megaBufferBinding)
                    builder.SetVertexBuffer(index, megaBufferBinding, ctx.gpu.traits.supportsExtendedDynamicState, engine->vertexStream.format.stride);
                else
                    builder.SetVertexBuffer(index, *view, ctx.gpu.traits.supportsExtendedDynamicState, engine->vertexStream.format.stride);
            }
        }
        return false;
    }

    void VertexBufferState::PurgeCaches() {
        view.PurgeCaches();
        megaBufferBinding = {};
    }

    /* Index Buffer Helpers */
    static vk::DeviceSize GetIndexBufferSize(engine::IndexBuffer::IndexSize indexSize, u32 elementCount) {
        switch (indexSize) {
            case engine::IndexBuffer::IndexSize::OneByte:
                return elementCount * 1;
            case engine::IndexBuffer::IndexSize::TwoBytes:
                return elementCount * 2;
            case engine::IndexBuffer::IndexSize::FourBytes:
                return elementCount * 4;
            default:
                throw exception("Unsupported index size enum value: {}", static_cast<u32>(indexSize));
        }
    }

    static vk::IndexType ConvertIndexType(engine::IndexBuffer::IndexSize indexSize) {
        switch (indexSize) {
            case engine::IndexBuffer::IndexSize::OneByte:
                return vk::IndexType::eUint8EXT;
            case engine::IndexBuffer::IndexSize::TwoBytes:
                return vk::IndexType::eUint16;
            case engine::IndexBuffer::IndexSize::FourBytes:
                return vk::IndexType::eUint32;
            default:
                throw exception("Unsupported index size enum value: {}", static_cast<u32>(indexSize));
        }
    }

    static BufferBinding GenerateQuadConversionIndexBuffer(InterconnectContext &ctx, engine::IndexBuffer::IndexSize indexType, BufferView &view, u32 firstIndex, u32 elementCount) {
        auto viewSpan{view.GetReadOnlyBackingSpan(false /* We attach above so always false */, []() {
            // TODO: see Read()
            LOGE("Dirty index buffer reads for attached buffers are unimplemented");
        })};

        size_t indexSize{1U << static_cast<u32>(indexType)};
        vk::DeviceSize indexBufferSize{conversion::quads::GetRequiredBufferSize(elementCount, indexSize)};
        auto quadConversionAllocation{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, indexBufferSize)};

        conversion::quads::GenerateIndexedQuadConversionBuffer(quadConversionAllocation.region.data(), viewSpan.subspan(GetIndexBufferSize(indexType, firstIndex)).data
