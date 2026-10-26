// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include "vic.h"

namespace skyline::soc::host1x {
    VicClass::VicClass(std::function<void()> opDoneCallback, const DeviceState &state)
        : state{state}, opDoneCallback(std::move(opDoneCallback)) {}

    void VicClass::CallMethod(u32 method, u32 argument) {
        if (!reportedUnsupported) {
            reportedUnsupported = true;
            LOGW("VIC is unimplemented: video jobs currently produce no output image");
        }
        capture.Method(method, argument, [this](u8 *destination, u64 address, u32 size) {
            state.soc->smmu.Read(destination, address, size);
        });
        if (method == VideoCapture::ExecuteMethod)
            throw exception("VIC execution unavailable: no output surface was produced");
    }
}
