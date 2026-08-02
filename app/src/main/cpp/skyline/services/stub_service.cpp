// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project
#include <mutex>
#include <unordered_set>
#include "stub_service.h"

namespace skyline::service {

    namespace {
        // Tracks (service name, cmdId) pairs that have already been logged, so a hot
        // IPC call (e.g. polled every frame by a game) only pays the logcat I/O cost
        // once instead of on every single invocation. Repeated unlogged calls were
        // causing measurable frame pacing stutter despite a stable average FPS.
        std::mutex loggedStubCallsMutex;
        std::unordered_set<std::string> loggedStubCalls;

        bool ShouldLog(std::string_view serviceName, u32 cmdId) {
            std::string key{serviceName};
            key += ':';
            key += std::to_string(cmdId);

            std::scoped_lock lock{loggedStubCallsMutex};
            return loggedStubCalls.insert(std::move(key)).second;
        }
    }

    Result StubService::StubFunction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (ShouldLog(serviceName, request.payload->value))
            LOGW("[STUB] Service '{}' received unimplemented call (cmdId=0x{:X}, tipc={}) — further calls to this cmdId will not be logged",
                 serviceName, request.payload->value, request.isTipc);
        // Return HOS error: Module=1 (Kernel), Description=0x601 (NotImplemented)
        return Result{0xF601};
    }

    BaseService::ServiceFunctionDescriptor StubService::GetServiceFunction(u32 id, bool isTipc) {
        return BaseService::ServiceFunctionDescriptor{
            reinterpret_cast<BaseService::DerivedService*>(this),
            reinterpret_cast<decltype(BaseService::ServiceFunctionDescriptor::function)>(&StubService::StubFunction),
            "StubService::StubFunction"
        };
    }
}