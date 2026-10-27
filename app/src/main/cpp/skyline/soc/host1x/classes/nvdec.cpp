// SPDX-License-Identifier: MPL-2.0
#include <soc/host1x/video/skyline_memory.h>
#include "nvdec.h"
namespace skyline::soc::host1x {
    NvDecClass::NvDecClass(std::function<void()> callback, const DeviceState &state)
        : state(state), opDoneCallback(std::move(callback)) {}
    void NvDecClass::CallMethod(u32 method, u32 argument) {
        capture.Method(method,argument,[this](u8 *destination,u64 address,u32 size) {
            state.soc->smmu.Read(destination,address,size);
        });
        try {
            engine.Method(method,argument,video::GuestMemory(state));
            if (method==VideoCapture::ExecuteMethod) opDoneCallback();
        } catch (const std::exception &error) {
            LOGE("NVDEC execution failed (method 0x{:X}): {}",method,error.what());
            throw;
        }
    }
}
