// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace skyline::service::fssrv {
    // Only inspect a bounded, complete ftyp box at the start of the bytes returned.
    // A brand identifies a candidate container, never a video track or playback intent.
    inline bool IsMp4Candidate(std::span<const std::uint8_t> bytes) {
        if (bytes.size() < 16)
            return false;
        const auto tag = [&](std::size_t i) {
            return std::string_view(reinterpret_cast<const char *>(bytes.data() + i), 4);
        };
        if (tag(4) != "ftyp")
            return false;
        const auto length = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16)
                            | (std::uint32_t{bytes[2]} << 8) | bytes[3];
        if (length < 16 || length > 256 || length > bytes.size() || length % 4)
            return false;
        for (std::size_t i = 8; i < length; i += 4) {
            if (i == 12) continue; // minor_version
            const auto brand = tag(i);
            if (brand == "isom" || brand == "iso2" || brand == "mp41" || brand == "mp42" || brand == "avc1")
                return true;
        }
        return false;
    }
}
