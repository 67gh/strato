// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include <soc/host1x/video_capture.h>
#include <soc/host1x/video/decoder.h>

namespace skyline::soc::host1x {
    /**
     * @brief NVDEC software decoding: progressive H264 and VP8, with diagnostic capture
     */
    class NvDecClass {
      private:
        const DeviceState &state;
        VideoCapture capture{VideoCapture::Engine::Nvdec};
        video::Decoder engine;
        std::function<void()> opDoneCallback;

      public:
        NvDecClass(std::function<void()> opDoneCallback, const DeviceState &state);

        void CallMethod(u32 method, u32 argument);
    };
}
