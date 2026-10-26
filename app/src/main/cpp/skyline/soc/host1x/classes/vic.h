// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include <soc/host1x/video_capture.h>

namespace skyline::soc::host1x {
    /**
     * @brief Diagnostic register capture for VIC; execution is explicitly unsupported
     */
    class VicClass {
      private:
        const DeviceState &state;
        VideoCapture capture{VideoCapture::Engine::Vic};
        bool reportedUnsupported{};
        std::function<void()> opDoneCallback;

      public:
        VicClass(std::function<void()> opDoneCallback, const DeviceState &state);

        void CallMethod(u32 method, u32 argument);
    };
}
