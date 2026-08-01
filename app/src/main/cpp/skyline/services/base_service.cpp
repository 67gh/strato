// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Modified 2024: Strato Revival Project — removed exception-based crash paths

#include <cxxabi.h>
#include <common/trace.h>
#include "base_service.h"

namespace skyline::service {
    Result service::BaseService::HandleRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        ServiceFunctionDescriptor function;
        u32 functionId{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};

        function = GetServiceFunction(functionId, request.isTipc);
        if (function.clazz == nullptr) {
            LOGW("[STUB] No valid function found in service '{}' for {} cmdId=0x{:X} ({})",
                 GetName(), request.isTipc ? "TIPC" : "HIPC", functionId, functionId);
            return Result{0xF601};
        }

        LOGDNF("Service: {}", function.name);
        TRACE_EVENT("service", perfetto::StaticString{function.name});
        try {
            return function(session, request, response);
        } catch (exception &e) {
            // We need to forward any skyline::exception objects without modification even though they inherit from std::exception
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            throw exception("{} (Service: {})", e.what(), function.name);
        } catch (...) {
            // Anti-crash: catch any non-std::exception (e.g. thrown from driver/vendor code) instead of
            // letting it propagate to std::terminate() and killing the whole emulator process.
            LOGE("[STUB] Unknown non-standard exception caught in service '{}' (Function: {}) — returning NotImplemented instead of crashing", GetName(), function.name);
            return Result{0xF601};
        }
    }
}
