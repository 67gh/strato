// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace skyline::soc::host1x {
    // Validate an entire gather before emitting any method. The class is channel
    // state: a gather boundary does not itself select the Host1x class.
    template<typename Emit>
    std::uint16_t ParseCommandBuffer(std::span<const std::uint32_t> words,
                                    std::uint16_t initialClass, Emit &&emit) {
        auto walk = [&](bool execute) {
            auto targetClass{initialClass};
            std::size_t cursor{};
            while (cursor < words.size()) {
                const auto header{words[cursor++]};
                const auto opcode{header >> 28};
                const auto method{(header >> 16) & 0xfffU};
                const auto data{header & 0xffffU};
                std::uint32_t mask{}, count{};
                switch (opcode) {
                    case 0: // SETCLASS
                        targetClass = static_cast<std::uint16_t>((data >> 6) & 0x3ffU);
                        mask = data & 0x3fU;
                        count = static_cast<std::uint32_t>(std::popcount(mask));
                        break;
                    case 1: // INCR
                    case 2: // NONINCR
                        count = data;
                        break;
                    case 3: // MASK
                        mask = data;
                        count = static_cast<std::uint32_t>(std::popcount(mask));
                        break;
                    case 4: // IMM has a full 16-bit payload.
                        if (execute)
                            emit(targetClass, method, data);
                        continue;
                    default:
                        throw std::invalid_argument("Unsupported Host1x command opcode");
                }
                if (count > words.size() - cursor)
                    throw std::invalid_argument("Truncated Host1x command payload");
                if (execute) {
                    if (opcode == 0 || opcode == 3) {
                        for (std::uint32_t bit{}; bit < 16; ++bit)
                            if (mask & (1U << bit))
                                emit(targetClass, method + bit, words[cursor++]);
                    } else {
                        for (std::uint32_t index{}; index < count; ++index)
                            emit(targetClass, method + (opcode == 1 ? index : 0U), words[cursor++]);
                    }
                } else {
                    cursor += count;
                }
            }
            return targetClass;
        };
        walk(false);
        return walk(true);
    }
}
