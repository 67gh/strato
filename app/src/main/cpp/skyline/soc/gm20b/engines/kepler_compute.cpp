// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <soc/gm20b/channel.h>
#include "kepler_compute/qmd.h"
#include "kepler_compute.h"

namespace skyline::soc::gm20b::engine {
    static gpu::interconnect::kepler_compute::KeplerCompute::EngineRegisterBundle MakeEngineRegisters(const KeplerCompute::Registers &registers) {
        return {
            .pipelineStateRegisters = {*registers.programRegion, *registers.bindlessTexture},
            .samplerPoolRegisters = {*registers.texSamplerPool, *registers.texHeaderPool},
            .texturePoolRegisters = {*registers.texHeaderPool}
        };
    }

    KeplerCompute::KeplerCompute(const DeviceState &state, ChannelContext &channelCtx)
        : syncpoints{state.soc->host1x.syncpoints},
          channelCtx{channelCtx},
          i2m{state, channelCtx},
          dirtyManager{registers},
          interconnect{*state.gpu, channelCtx, state.process->trap, state.process->memory, dirtyManager, MakeEngineRegisters(registers)} {}

    __attribute__((always_inline)) void KeplerCompute::CallMethod(u32 method, u32 argument) {
        LOGV("Called method in Kepler compute: 0x{:X} args: 0x{:X}", method, argument);

        HandleMethod(method, argument);
    }

    void KeplerCompute::HandleMethod(u32 method, u32 argument) {
        registers.raw[method] = argument;

        switch (method) {
            ENGINE_STRUCT_CASE(i2m, launchDma, {
                i2m.LaunchDma(*registers.i2m);
            })
            ENGINE_STRUCT_CASE(i2m, loadInlineData, {
                i2m.LoadInlineData(*registers.i2m, argument);
            })
            ENGINE_CASE(sendSignalingPcasB, {
                interconnect.Dispatch(channelCtx.asCtx->gmmu.Read<kepler_compute::QMD>(registers.sendPcas->QmdAddress()));
            })
            ENGINE_STRUCT_CASE(reportSemaphore, action, {
                // Compute semaphore release: write the payload to the semaphore address.
                // NVN uses these to synchronise compute dispatches with the 3D engine.
                // We signal completion immediately (optimistic — no actual GPU fence wait)
                // which is safe because Strato's executor serialises GPU work anyway.
                u64 address{registers.reportSemaphore->address};
                u32 payload{registers.reportSemaphore->payload};

                switch (registers.reportSemaphore->op) {
                    case Registers::ReportSemaphore::Op::Release:
                        // Write 32-bit payload
                        channelCtx.asCtx->gmmu.Write(address, payload);
                        LOGD("Compute semaphore release: addr=0x{:X} payload=0x{:X}", address, payload);
                        break;
                    case Registers::ReportSemaphore::Op::Counter:
                        // Write 64-bit {payload, timestamp} — some NVN titles check the counter
                        channelCtx.asCtx->gmmu.Write(address, static_cast<u64>(payload));
                        LOGD("Compute semaphore counter: addr=0x{:X} payload=0x{:X}", address, payload);
                        break;
                    default:
                        LOGW("Unhandled compute semaphore op: {}", static_cast<u32>(registers.reportSemaphore->op));
                        break;
                }
            })
            default:
                return;
        }
    }

    void KeplerCompute::CallMethodBatchNonInc(u32 method, span<u32> arguments) {
        switch (method) {
            case ENGINE_STRUCT_OFFSET(i2m, loadInlineData):
                i2m.LoadInlineData(*registers.i2m, arguments);
                return;
            default:
                break;
        }

        for (u32 argument : arguments)
            HandleMethod(method, argument);
    }
}
