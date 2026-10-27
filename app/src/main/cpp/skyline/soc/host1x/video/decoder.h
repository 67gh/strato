// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <memory>
#include "memory.h"
namespace skyline::soc::host1x::video {
class Decoder {
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::array<std::uint32_t,0x446> regs{};
    void Execute(const Memory &memory);
public:
    Decoder();
    ~Decoder();
    void Method(std::uint32_t method, std::uint32_t value, const Memory &memory);
};
}
