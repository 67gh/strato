// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project
#include "stub_service.h"

namespace skyline::service {

    Result StubService::StubFunction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGW("[STUB] Service '{}' received unimplemented call (cmdId=0x{:X}, tipc={})",
             serviceName, request.payload->value, request.isTipc);
        // Return HOS error: Module=1 (Kernel), Description=0x601 (NotImplemented)
        return Result{0xF601};
    }

    ServiceFunctionDescriptor StubService::GetServiceFunction(u32 id, bool isTipc) {
        return ServiceFunctionDescriptor{
            reinterpret_cast<DerivedService*>(this),
            reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(&StubService::StubFunction),
            "StubService::StubFunction"
        };
    }
}
