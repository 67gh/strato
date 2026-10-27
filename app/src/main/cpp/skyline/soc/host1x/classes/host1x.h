// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include <soc/host1x/syncpoint.h>

namespace skyline::soc::host1x {
    /**
     * @brief A class internal to Host1x, used for performing syncpoint waits and other general operations
     */
    class Host1xClass {
      private:
        SyncpointSet &syncpoints;
        u32 syncpointPayload{}; //!< Holds the current payload for the 32-bit syncpoint comparison methods

        std::chrono::milliseconds waitTimeout{5000};
      public:
        void SetWaitTimeout(std::chrono::milliseconds timeout) { waitTimeout=timeout; }
        Host1xClass(SyncpointSet &syncpoints);

        void CallMethod(u32 method, u32 argument);
    };
}
