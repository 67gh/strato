// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project
#pragma once

#include "base_service.h"

namespace skyline::service {
    /**
     * @brief Stub service that replies to any IPC call with ResultNotImplemented.
     *        Prevents crashes when a game (e.g. TOTK) requests an unimplemented service.
     */
    class StubService : public BaseService {
      public:
        StubService(const DeviceState &state, ServiceManager &manager, std::string svcName)
            : BaseService(state, manager), serviceName(std::move(svcName)) {}

        Result StubFunction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) override;

        const std::string &GetName() override {
            return serviceName;
        }

      private:
        std::string serviceName;
    };
}
