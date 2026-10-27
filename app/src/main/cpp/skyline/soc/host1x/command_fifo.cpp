// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/signal.h>
#include <nce.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include <soc.h>
#include "command_fifo.h"
#include "command_parser.h"

namespace skyline::soc::host1x {
    ChannelCommandFifo::ChannelCommandFifo(const DeviceState &state, SyncpointSet &syncpoints) : state(state), gatherQueue(GatherQueueSize), host1XClass(syncpoints), nvDecClass(syncpoints, state), vicClass(syncpoints, state) {}

    void ChannelCommandFifo::Send(ClassId targetClass, u32 method, u32 argument) {
        LOGV("Calling method in class: 0x{:X}, method: 0x{:X}, argument: 0x{:X}", targetClass, method, argument);

        switch (targetClass) {
            case ClassId::Host1x:
                host1XClass.CallMethod(method, argument);
                break;
            case ClassId::NvDec:
                nvDecClass.CallMethod(method, argument);
                break;
            case ClassId::VIC:
                vicClass.CallMethod(method, argument);
                break;
            default:
                throw exception("Unsupported Host1x class: 0x{:X}", static_cast<u16>(targetClass));
        }
    }

    void ChannelCommandFifo::Validate(span<u32> gather) {
        ParseCommandBuffer(std::span<const u32>{gather.data(),gather.size()},static_cast<u16>(targetClass),
            [](u16 cls,u32 method,u32 argument) {
                if (cls!=static_cast<u16>(ClassId::Host1x) && cls!=static_cast<u16>(ClassId::NvDec) && cls!=static_cast<u16>(ClassId::VIC))
                    throw exception("Unsupported video class: {}",cls);
                if (cls==static_cast<u16>(ClassId::Host1x)) {
                    if (method!=0 && method!=0x4e && method!=0x50)
                        throw exception("Unsupported Host1x method: {}",method);
                    if (method==0x50 && (argument&0xffU)>=SyncpointCount)
                        throw exception("Invalid Host1x wait syncpoint");
                } else if (method!=0 && method!=0x10 && method!=0x11) {
                    throw exception("Unsupported THI method: {}",method);
                }
                if (!method && (argument&0xffU)>=SyncpointCount)
                    throw exception("Invalid video syncpoint");
            });
    }

    void ChannelCommandFifo::Process(span<u32> gather) {
        targetClass = static_cast<ClassId>(ParseCommandBuffer(
            std::span<const u32>{gather.data(), gather.size()}, static_cast<u16>(targetClass),
            [this](u16 classId, u32 method, u32 argument) {
                Send(static_cast<ClassId>(classId), method, argument);
            }));
    }

    void ChannelCommandFifo::Start() {
        std::scoped_lock lock(threadStartMutex);

        if (!thread.joinable())
            thread = std::thread(&ChannelCommandFifo::Run, this);
    }

    void ChannelCommandFifo::Run() {
        if (int result{pthread_setname_np(pthread_self(), "ChannelCmdFifo")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            gatherQueue.Process([this](std::vector<u32> &gather) {
                LOGD("Processing pushbuffer: {}, size: 0x{:X}", fmt::ptr(gather.data()), gather.size());
                Process(span<u32>{gather.data(), gather.size()});
            }, [] {});
        } catch (const signal::SignalException &e) {
            if (e.signal != SIGINT) {
                LOGE("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }
        } catch (const exception &e) {
            LOGENF("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            signal::BlockSignal({SIGINT});
            state.process->Kill(false);
        } catch (const std::exception &e) {
            LOGE("{}", e.what());
            signal::BlockSignal({SIGINT});
            state.process->Kill(false);
        }
    }

    void ChannelCommandFifo::Push(span<u32> gather) {
        if (gather.empty())
            return;
        // The caller may unmap or reuse the original memory as soon as Push returns.
        gatherQueue.Push(std::vector<u32>(gather.begin(), gather.end()));
    }

    ChannelCommandFifo::~ChannelCommandFifo() {
        if (thread.joinable()) {
            pthread_kill(thread.native_handle(), SIGINT);
            thread.join();
        }
    }
}
