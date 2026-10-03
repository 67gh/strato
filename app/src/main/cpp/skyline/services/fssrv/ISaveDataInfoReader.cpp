// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISaveDataInfoReader.h"

namespace skyline::service::fssrv {
    ISaveDataInfoReader::ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result ISaveDataInfoReader::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGD("Reporting no savedata entries");
        response.Push<u64>(0); // Number of SaveDataInfo entries written to the output buffer
        return {};
    }
}
