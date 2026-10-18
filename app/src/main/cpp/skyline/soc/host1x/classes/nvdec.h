// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include <soc/host1x/video_capture.h>

namespace skyline::soc::host1x {
    /**
     * @brief The NVDEC Host1x class implements hardware accelerated video decoding for the VP9/VP8/H264/VC1 codecs
     */
    class NvDecClass {
      private:
        const DeviceState &state;
        VideoCapture capture{VideoCapture::Engine::Nvdec};
        bool reportedUnsupported{};
        std::function<void()> opDoneCallback;

      public:
        NvDecClass(std::function<void()> opDoneCallback, const DeviceState &state);

        void CallMethod(u32 method, u32 argument);
    };
}
