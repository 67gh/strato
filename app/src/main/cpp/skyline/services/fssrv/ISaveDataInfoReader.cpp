// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISaveDataInfoReader.h"

namespace skyline::service::fssrv {
    ISaveDataInfoReader::ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result ISaveDataInfoReader::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Return 0 entries — no save data exists yet.
        // The output buffer receives SaveDataInfo structs; writing count=0 is valid
        // and signals to the caller that enumeration is complete.
        response.Push<u64>(0); // count of entries written
        LOGD("ISaveDataInfoReader::Read — returning 0 entries");
        return {};
    }
}
