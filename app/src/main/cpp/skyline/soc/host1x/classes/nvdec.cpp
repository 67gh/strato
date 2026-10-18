// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include "nvdec.h"

namespace skyline::soc::host1x {
    NvDecClass::NvDecClass(std::function<void()> opDoneCallback, const DeviceState &state)
        : state{state}, opDoneCallback(std::move(opDoneCallback)) {}

    void NvDecClass::CallMethod(u32 method, u32 argument) {
        if (!reportedUnsupported) {
            reportedUnsupported = true;
            LOGW("NVDEC is unimplemented: video jobs currently produce no output image");
        }
        capture.Method(method, argument, [this](u8 *destination, u64 address, u32 size) {
            state.soc->smmu.Read(destination, address, size);
        });
    }
}
