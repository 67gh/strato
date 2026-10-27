// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <soc.h>
#include "memory.h"
namespace skyline::soc::host1x::video {
inline Memory GuestMemory(const DeviceState &state) {
    return {
        [&state](std::uint64_t address, std::span<std::uint8_t> bytes) {
            state.soc->smmu.Read(bytes.data(),address,bytes.size());
        },
        [&state](std::uint64_t address, std::span<const std::uint8_t> bytes) {
            state.soc->smmu.Write(address,const_cast<std::uint8_t *>(bytes.data()),bytes.size());
        },
        [&state](std::uint64_t address, std::size_t size) {
            for (const auto &range : state.soc->smmu.TranslateRange(address,size))
                if (!range.valid()) throw exception("Video: unmapped SMMU range 0x{:X}+0x{:X}",address,size);
        }
    };
}
}
