// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISystemSettingsServer.h"

namespace skyline::service::settings {
    ISystemSettingsServer::ISystemSettingsServer(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result ISystemSettingsServer::GetFirmwareVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Report firmware 22.1.0 — the latest Switch firmware as of 2024.
        // Strato does not require actual firmware files; this value is returned
        // purely to satisfy games that check the version via GetFirmwareVersion.
        request.outputBuf.at(0).as<SysVerTitle>() = {
            .major = 22, .minor = 1, .micro = 0,
            .revMajor = 0, .revMinor = 0,
            .platform = "NX",
            .verHash = "0000000000000000000000000000000000000000",
            .dispVer = "22.1.0",
            .dispTitle = "NintendoSDK Firmware for NX 22.1.0-0.0"
        };
        return {};
    }

    Result ISystemSettingsServer::GetColorSetId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0); // Basic White
        return {};
    }
}
