// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "memory.h"
namespace skyline::soc::host1x::video {
class Compositor {
    std::array<std::uint32_t,0x446> regs{};
    void Execute(const Memory &memory);
public:
    void Method(std::uint32_t method, std::uint32_t value, const Memory &memory);
};
}
