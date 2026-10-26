// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <limits>
#include <common/diagnostic_recorder.h>
#include <services/nvdrv/devices/deserialisation/deserialisation.h>
#include "host1x_channel.h"

namespace skyline::service::nvdrv::device::nvhost {
    Host1xChannel::Host1xChannel(const DeviceState &state,
                                 Driver &driver,
                                 Core &core,
                                 const SessionContext &ctx,
                                 core::ChannelType channelType)
        : NvDevice(state, driver, core, ctx),
          channelType(channelType) {}

    PosixResult Host1xChannel::SetNvmapFd(In<FileDescriptor> fd) {
        LOGD("fd: {}", fd);
        // Compatibility no-op: all channel handles resolve through Core's shared NvMap.
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::Submit(span<SubmitCmdBuf> cmdBufs,
                                      span<SubmitReloc> relocs, span<u32> relocShifts,
                                      span<SubmitSyncpointIncr> syncpointIncrs, span<u32> fenceThresholds) {
        LOGD("numCmdBufs: {}, numRelocs: {}, numSyncpointIncrs: {}, numFenceThresholds: {}",
                            cmdBufs.size(), relocs.size(), syncpointIncrs.size(), fenceThresholds.size());

        if (fenceThresholds.size() > syncpointIncrs.size() || relocs.size() != relocShifts.size())
            return PosixResult::InvalidArgument;

        std::scoped_lock lock(channelMutex);
        // Validate without modifying counters or output fences. A rejected submission
        // must not make an earlier/later job appear complete.
        for (const auto &incr : syncpointIncrs) {
            if (!core.syncpointManager.IsSyncpointAllocated(incr.syncpointId))
                return PosixResult::InvalidArgument;
        }
        for (const auto &cmdBuf : cmdBufs) {
            auto handleDesc{core.nvMap.GetHandle(cmdBuf.mem)};
            if (!handleDesc)
                return PosixResult::InvalidArgument;
            std::scoped_lock handleLock(handleDesc->mutex);
            if (!handleDesc->allocated || !handleDesc->address ||
                (handleDesc->address & (alignof(u32) - 1)) || (cmdBuf.offset & (alignof(u32) - 1)) ||
                cmdBuf.offset > handleDesc->size ||
                static_cast<u64>(cmdBuf.words) > (handleDesc->size - cmdBuf.offset) / sizeof(u32))
                return PosixResult::InvalidArgument;
            const u64 bytes{static_cast<u64>(cmdBuf.words) * sizeof(u32)};
            const u64 maxAddress{std::numeric_limits<u64>::max()};
            if (cmdBuf.offset > maxAddress - handleDesc->address ||
                bytes > maxAddress - (handleDesc->address + cmdBuf.offset))
                return PosixResult::InvalidArgument;
        }

        if (cmdBufs.empty() && relocs.empty() && syncpointIncrs.empty())
            return PosixResult::Success;

        // There is no execution backend for these channels yet. Do not enqueue
        // borrowed pointers, read guest memory, or fabricate completion on the CPU.
        // Returning an error is intentional: callers must not consume stale surfaces.
        if (!reportedUnsupported) {
            reportedUnsupported = true;
            LOGW("Host1x channel {}: submit rejected; video execution/relocations are unsupported ({} command buffers)",
                 static_cast<u32>(channelType), cmdBufs.size());
            diagnostics::Unsupported("host1x_submit", static_cast<u32>(channelType),
                                     "no execution backend; submission rejected without signalling fences");
        }
        return PosixResult::NotSupported;
    }

    PosixResult Host1xChannel::GetSyncpoint(In<u32> channelSyncpointIdx, Out<u32> syncpointId) {
        LOGD("channelSyncpointIdx: {}", channelSyncpointIdx);

        if (channelSyncpointIdx > 0)
            return PosixResult::InvalidArgument;

        if (static_cast<size_t>(channelType) >= core::SyncpointManager::ChannelSyncpoints.size())
            return PosixResult::InvalidArgument;
        u32 id{core::SyncpointManager::ChannelSyncpoints[static_cast<u32>(channelType)]};
        if (!id)
            return PosixResult::NotSupported;

        LOGD("syncpointId: {}", id);
        syncpointId = id;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::GetWaitBase(In<core::ChannelType> pChannelType, Out<u32> waitBase) {
        LOGD("channelType: {}", static_cast<u32>(pChannelType));
        waitBase = 0;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::SetSubmitTimeout(In<u32> timeout) {
        LOGD("timeout: {}", timeout);
        std::scoped_lock lock(channelMutex);
        submitTimeout = timeout;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::MapBuffer(u8 compressed, span<BufferHandle> handles) {
        LOGD("compressed: {}", compressed);

        for (auto &bufferHandle : handles) {
            bufferHandle.address = core.nvMap.PinHandle(bufferHandle.handle);
            LOGD("handle: {}, address: 0x{:X}", bufferHandle.handle, bufferHandle.address);
        }

        return PosixResult::Success;
    }

    PosixResult Host1xChannel::UnmapBuffer(u8 compressed, span<BufferHandle> handles) {
        LOGD("compressed: {}", compressed);

        for (auto &bufferHandle : handles) {
            core.nvMap.UnpinHandle(bufferHandle.handle);
            LOGD("handle: {}", bufferHandle.handle);
        }

        return PosixResult::Success;
    }

// @fmt:off
#include <services/nvdrv/devices/deserialisation/macro_def.inc>
    static constexpr u32 Host1xChannelMagic{0x00};
    static constexpr u32 GpuChannelMagic{0x48}; //!< Used for SetNvmapFd which is needed in both GPU and host1x channels

    VARIABLE_IOCTL_HANDLER_FUNC(Host1xChannel,
        IOCTL_CASE_ARGS(IN,    SIZE(0x4), MAGIC(GpuChannelMagic),    FUNC(0x1),
                        SetNvmapFd,       ARGS(In<FileDescriptor>))
        IOCTL_CASE_ARGS(INOUT, SIZE(0x8), MAGIC(Host1xChannelMagic), FUNC(0x2),
                        GetSyncpoint,     ARGS(In<u32>, Out<u32>))
        IOCTL_CASE_ARGS(INOUT, SIZE(0x8), MAGIC(Host1xChannelMagic), FUNC(0x3),
                        GetWaitBase,      ARGS(In<core::ChannelType>, Out<u32>))
        IOCTL_CASE_ARGS(IN,    SIZE(0x4), MAGIC(Host1xChannelMagic), FUNC(0x7),
                        SetSubmitTimeout, ARGS(In<u32>))
    ,
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0x1),
                                 Submit,      ARGS(Save<u32, 0>, Save<u32, 1>, Save<u32, 2>, Save<u32, 3>,
                                                   SlotSizeSpan<SubmitCmdBuf, 0>,
                                                   SlotSizeSpan<SubmitReloc, 1>, SlotSizeSpan<u32, 1>,
                                                   SlotSizeSpan<SubmitSyncpointIncr, 2>, SlotSizeSpan<u32, 3>))
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0x9),
                                 MapBuffer,   ARGS(Save<u32, 0>, Pad<u32>, In<u8>, Pad<u8, 3>, SlotSizeSpan<BufferHandle, 0>))
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0xA),
                                 UnmapBuffer, ARGS(Save<u32, 0>, Pad<u32>, In<u8>, Pad<u8, 3>, SlotSizeSpan<BufferHandle, 0>))
    )
#include <services/nvdrv/devices/deserialisation/macro_undef.inc>
// @fmt:on
}
