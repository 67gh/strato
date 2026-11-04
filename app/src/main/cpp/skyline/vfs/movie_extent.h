// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace skyline::vfs {
    // Rows of the little-endian RomFS file metadata table have a 32-byte fixed
    // part, followed by a four-byte-aligned name. Never follow untrusted links.
    inline std::optional<std::uint64_t> MovieExtent(std::span<const std::uint8_t> table,
            std::uint64_t dataSize, std::uint64_t wanted) {
        std::optional<std::uint64_t> size;
        auto nextFile = dataSize;
        if (wanted >= dataSize) return {};
        while (!table.empty()) {
            if (table.size() < 32) return {};
            auto word = [&](unsigned offset, unsigned bytes) {
                std::uint64_t value{};
                for (unsigned i = 0; i < bytes; ++i)
                    value |= std::uint64_t{table[offset + i]} << (8 * i);
                return value;
            };
            const auto start = word(8, 8), length = word(16, 8);
            const auto name = word(28, 4), padded = (name + 3) & ~std::uint64_t{3};
            if (!name || padded > table.size() - 32 || start > dataSize || length > dataSize - start)
                return {};
            if (start < wanted && length > wanted - start) return {};
            if (start > wanted && length && start < nextFile) nextFile = start;
            if (start == wanted && length) {
                if (size) return {};
                size = length;
            }
            table = table.subspan(32 + padded);
        }
        if (size && *size > nextFile - wanted) return {};
        return size;
    }
}
