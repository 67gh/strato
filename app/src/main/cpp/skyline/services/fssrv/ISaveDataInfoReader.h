// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::fssrv {

    /**
     * @url https://switchbrew.org/wiki/Filesystem_services#ISaveDataInfoReader
     */
    class ISaveDataInfoReader : public BaseService {
      public:
        ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager);

        /**
         * @brief Read (0x0) — Returns an empty list of save data entries.
         * @url https://switchbrew.org/wiki/Filesystem_services#ISaveDataInfoReader
         * MK11 calls this to enumerate save data before init. Returning an empty
         * list (count=0) is correct when no save data exists yet.
         */
        Result Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ISaveDataInfoReader, Read)
        )
    };
}
