// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project
#include <common/diagnostic_recorder.h>
#include "stub_service.h"

namespace skyline::service {
    Result StubService::StubFunction(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const u32 command{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};
        diagnostics::Unsupported(serviceName, command, "stub service; result=0xF601");
        LOGW("[STUB] Service '{}' cmd=0x{:X} tipc={} result=0xF601", serviceName, command, request.isTipc);
        return Result{0xF601};
    }
    BaseService::ServiceFunctionDescriptor StubService::GetServiceFunction(u32, bool) {
        return BaseService::ServiceFunctionDescriptor{
            reinterpret_cast<BaseService::DerivedService*>(this),
            reinterpret_cast<decltype(BaseService::ServiceFunctionDescriptor::function)>(&StubService::StubFunction),
            "StubService::StubFunction"
        };
    }
}
