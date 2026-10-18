// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Modified 2024: Strato Revival Project — removed exception-based crash paths

#include <cxxabi.h>
#include <array>
#include <cstring>
#include <algorithm>
#include <common/trace.h>
#include "base_service.h"
#include "diagnostic_noops.h"

namespace skyline::service {
    Result service::BaseService::HandleRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        ServiceFunctionDescriptor function;
        u32 functionId{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};

        diagnostics::Scope diagnosticScope(0);
        diagnostics::Event("ipc_request", functionId, request.cmdArgSz, request.isTipc, GetName());
        diagnostics::Operation operation("ipc_call", functionId, diagnostics::Timing::Ipc);
        if (diagnostics::RecorderEnabled() && AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Verbose)) {
            std::array<u64,4> words{};
            const auto base=reinterpret_cast<uintptr_t>(request.header), args=reinterpret_cast<uintptr_t>(request.cmdArg);
            size_t bytes{};
            if (args>=base && args-base<constant::TlsIpcSize)
                bytes=std::min({static_cast<size_t>(request.cmdArgSz),sizeof(words),static_cast<size_t>(constant::TlsIpcSize-(args-base))});
            if(bytes)std::memcpy(words.data(),request.cmdArg,bytes);
            diagnostics::Event("ipc_arguments",functionId,bytes,request.cmdArgSz,
                fmt::format("raw_le_words={:X},{:X},{:X},{:X}; buffers_in={} out={}",words[0],words[1],words[2],words[3],request.inputBuf.size(),request.outputBuf.size()));
        }
        auto finish=[&](Result result) {
            diagnostics::Event("ipc_result",functionId,static_cast<u32>(result),0,GetName());
            operation.Finish(static_cast<u32>(result));
            if (static_cast<u32>(result)) LOGW("Service '{}' cmd=0x{:X} returned 0x{:X}",GetName(),functionId,static_cast<u32>(result));
            return result;
        };
        function = GetServiceFunction(functionId, request.isTipc);
        if (function.clazz == nullptr) {
            diagnostics::Unsupported(GetName(), functionId, "missing service function");
            LOGW("[STUB] No valid function found in service '{}' for {} cmdId=0x{:X} ({})",
                 GetName(), request.isTipc ? "TIPC" : "HIPC", functionId, functionId);
            return finish(Result{0xF601});
        }

        if (diagnostics_detail::IsSuccessOnly(GetName(), function.name)) {
            diagnostics::Unsupported(GetName(), functionId, function.name);
            diagnostics::Event("ipc_success_only", functionId, 0, request.isTipc, function.name);
            LOGD("Service '{}' {}: body returns success without an action; may be intentional", GetName(), function.name);
        }
        LOGDNF("Service: {}", function.name);
        TRACE_EVENT("service", perfetto::StaticString{function.name});
        try {
            return finish(function(session, request, response));
        } catch (exception &e) {
            diagnostics::Event("ipc_exception",functionId,0,0,e.what());
            // We need to forward any skyline::exception objects without modification even though they inherit from std::exception
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            diagnostics::Event("ipc_exception",functionId,0,0,e.what());
            throw exception("{} (Service: {})", e.what(), function.name);
        } catch (...) {
            // Anti-crash: catch any non-std::exception (e.g. thrown from driver/vendor code) instead of
            // letting it propagate to std::terminate() and killing the whole emulator process.
            LOGE("[STUB] Unknown non-standard exception caught in service '{}' (Function: {}) — returning NotImplemented instead of crashing", GetName(), function.name);
            return finish(Result{0xF601});
        }
    }
}
