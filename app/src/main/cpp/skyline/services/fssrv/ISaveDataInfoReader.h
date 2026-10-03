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
         * @brief Reads SaveDataInfo entries (0x60 bytes each) into the output buffer and returns how many were written
         * @note No savedata is ever enumerated, an empty list is reported so the application treats this as having no existing savedata (games such as TotK abort on startup if this command isn't implemented)
         * @url https://switchbrew.org/wiki/Filesystem_services#Read
         */
        Result Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ISaveDataInfoReader, Read)
        )
    };
}
