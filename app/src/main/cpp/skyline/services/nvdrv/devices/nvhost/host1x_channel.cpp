// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <limits>
#include <soc/host1x/command_parser.h>
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
            if (!core.syncpointManager.IsSyncpointAllocated(incr.syncpointId) || incr.numIncrs>0x7fffffffU)
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

        if (channelType!=core::ChannelType::NvDec && channelType!=core::ChannelType::VIC)
            return PosixResult::NotSupported;
        if (executionFailed) return PosixResult::InvalidArgument;
        if (cmdBufs.empty() && relocs.empty() && syncpointIncrs.empty()) return PosixResult::Success;

        // Copy first; relocations patch this owned copy, never application memory.
        // A synchronous ioctl keeps all relocation pins alive through output writes.
        std::vector<u32> commands;
        std::vector<size_t> starts;
        for (const auto &cmd : cmdBufs) {
            if (cmd.words > 4*1024*1024 - commands.size()) return PosixResult::InvalidArgument;
            auto handle=core.nvMap.GetHandle(cmd.mem);
            if (!handle) return PosixResult::InvalidArgument;
            std::scoped_lock handleLock(handle->mutex);
            if (!handle->allocated || !handle->address || cmd.offset>handle->size ||
                static_cast<u64>(cmd.words)>(handle->size-cmd.offset)/sizeof(u32))
                return PosixResult::InvalidArgument;
            starts.push_back(commands.size());
            const auto begin=commands.size();
            commands.resize(begin+cmd.words);
            if (cmd.words) std::memcpy(commands.data()+begin,reinterpret_cast<const void *>(handle->address+cmd.offset),cmd.words*sizeof(u32));
        }
        if (commands.empty()) return PosixResult::InvalidArgument;
        struct Pins {
            core::NvMap &map;
            std::vector<u32> handles;
            ~Pins() { for (auto id : handles) map.UnpinHandle(id); }
        } pins{core.nvMap,{}};
        pins.handles.reserve(relocs.size());
        // Validate every relocation before pinning or changing any syncpoint.
        for (size_t i=0;i<relocs.size();++i) {
            const auto &reloc=relocs[i];
            auto target=core.nvMap.GetHandle(reloc.pinMem);
            if (!target || relocShifts[i]>31 || (reloc.patchOffset&3)) return PosixResult::InvalidArgument;
            std::scoped_lock targetLock(target->mutex);
            if (!target->allocated || !target->address || reloc.pinOffset>=target->size) return PosixResult::InvalidArgument;
            bool found{};
            for (const auto &cmd : cmdBufs)
                if (cmd.mem==reloc.patchMem && reloc.patchOffset>=cmd.offset &&
                    static_cast<u64>(reloc.patchOffset-cmd.offset)/4<cmd.words) found=true;
            if (!found) return PosixResult::InvalidArgument;
        }
        try {
            for (size_t i=0;i<relocs.size();++i) {
                const auto &reloc=relocs[i];
                const u64 base=core.nvMap.PinHandle(reloc.pinMem);
                if (!base) return PosixResult::InvalidArgument;
                pins.handles.push_back(reloc.pinMem);
                const u64 address=base+reloc.pinOffset;
                if (address>std::numeric_limits<u32>::max()) return PosixResult::InvalidArgument;
                for (size_t j=0;j<cmdBufs.size();++j) {
                    const auto &cmd=cmdBufs[j];
                    if (cmd.mem==reloc.patchMem && reloc.patchOffset>=cmd.offset &&
                        static_cast<u64>(reloc.patchOffset-cmd.offset)/4<cmd.words)
                        commands[starts[j]+(reloc.patchOffset-cmd.offset)/4]=static_cast<u32>(address>>relocShifts[i]);
                }
            }
            if (!videoChannel) videoChannel=std::make_unique<soc::host1x::ChannelCommandFifo>(state,state.soc->host1x.syncpoints);
            // Each gather must end on a packet boundary. Concatenating before this
            // check could make a truncated packet consume the next gather's header.
            for (size_t j=0;j<cmdBufs.size();++j)
                soc::host1x::ParseCommandBuffer(std::span<const u32>{commands.data()+starts[j],cmdBufs[j].words},1,
                                               [](u16,u32,u32) {});
            // Parse without dispatch before reserving counters, including malformed tails.
            videoChannel->Validate(span<u32>{commands.data(),commands.size()});
        } catch (const std::exception &error) {
            LOGE("Host1x submit validation failed: {}",error.what());
            return PosixResult::InvalidArgument;
        }
        std::vector<u32> thresholds;
        thresholds.reserve(syncpointIncrs.size());
        for (const auto &incr : syncpointIncrs)
            thresholds.push_back(core.syncpointManager.IncrementSyncpointMaxExt(incr.syncpointId,incr.numIncrs));
        try {
            videoChannel->SetWaitTimeout(std::chrono::milliseconds(submitTimeout ? submitTimeout : 5000));
            videoChannel->Process(span<u32>{commands.data(),commands.size()});
            for (size_t i=0;i<syncpointIncrs.size();++i) {
                const auto completed=core.syncpointManager.UpdateMin(syncpointIncrs[i].syncpointId);
                if (static_cast<i32>(completed-thresholds[i])<0)
                    throw exception("Host1x completed without requested syncpoint increments (id {})",syncpointIncrs[i].syncpointId);
            }
        } catch (const std::exception &error) {
            executionFailed=true;
            LOGE("Host1x video submit failed: {}; channel disabled; restart the emulation session",error.what());
            return PosixResult::InvalidArgument;
        }
        for (size_t i=0;i<fenceThresholds.size();++i) fenceThresholds[i]=thresholds[i];
        return PosixResult::Success;
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
