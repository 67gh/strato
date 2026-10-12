// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/diagnostic_recorder.h>
#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn) {
        skyline::diagnostics::Event("buffer_mapping_request", ctx.channelCtx.asCtx->gmmu.diagnosticSpaceId, address, size);
        if (!size) {
            view = {};
            return;
        }
        // Semaphore handling purges this cache after guest mapping changes.
        if (address < blockMappingStartAddr || address >= blockMappingEndAddr) {
            u64 blockOffset{};
            std::tie(blockMapping, blockOffset) = ctx.channelCtx.asCtx->gmmu.LookupBlock(address);
            if (!blockMapping.valid()) {
                skyline::diagnostics::Event("buffer_unmapped", ctx.channelCtx.asCtx->gmmu.diagnosticSpaceId, address, size);
                view = {};
                blockMappingEndAddr = 0;
                return;
            }

            blockMappingStartAddr = address - blockOffset;
            blockMappingEndAddr = blockMappingStartAddr + blockMapping.size();
        }

        // Mapping from the start of the buffer view to the end of the block
        auto fullMapping{blockMapping.subspan(address - blockMappingStartAddr)};

        if (fullMapping.size() < size) {
            // TranslateRange merges adjacent host ranges. A guest resource may
            // cross mapping boundaries while still having contiguous backing.
            auto &gmmu{ctx.channelCtx.asCtx->gmmu};
            auto ranges{gmmu.TranslateRange(address, std::min(size, gmmu.vaLimit - address))};
            if (!ranges.empty() && ranges.front().valid())
                fullMapping = ranges.front();
            if (splitMappingWarn && fullMapping.size() < size)
                LOGW("Buffer at 0x{:X}: contiguous mapped prefix 0x{:X} of requested 0x{:X} bytes", address, fullMapping.size(), size);
        }

        // Constant/index buffer callers may supply an estimated upper bound.
        // Exact-size resources (texel buffers) reject a shorter view themselves.
        if (fullMapping.size() < size) skyline::diagnostics::Event("buffer_partial_mapping", ctx.channelCtx.asCtx->gmmu.diagnosticSpaceId, address, fullMapping.size());
        skyline::diagnostics::Event("buffer_host_range", ctx.channelCtx.asCtx->gmmu.diagnosticSpaceId, reinterpret_cast<uintptr_t>(fullMapping.data()), std::min(fullMapping.size(), size));
        auto viewMapping{fullMapping.first(std::min(fullMapping.size(), size))};

        // First attempt to skip lookup by trying to reuse the previous view's underlying buffer
        if (view)
            if (view = view.GetBuffer()->TryGetView(viewMapping); view)
                return;

        // Otherwise perform a full lookup
        view = ctx.gpu.buffer.FindOrCreate(viewMapping, ctx.executor.tag, [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
            ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
        });
    }

    void CachedMappedBufferView::PurgeCaches() {
        view = {};
        blockMappingEndAddr = 0; // Will force a retranslate of `blockMapping` on the next `Update()` call
    }

    static void FlushHostCallback() {
        // TODO: here we should trigger `Execute()`, however that doesn't currently work due to Read being called mid-draw and attached objects not handling this case
        LOGW("GPU dirty buffer reads for attached buffers are unimplemented");
    }

    void ConstantBuffer::Read(CommandExecutor &executor, span<u8> dstBuffer, size_t srcOffset) {
        ContextLock lock{executor.tag, view};
        view.Read(lock.IsFirstUsage(), FlushHostCallback, dstBuffer, srcOffset);
    }
}
