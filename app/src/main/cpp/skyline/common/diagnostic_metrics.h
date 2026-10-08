// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace skyline::diagnostics {
    // Count real frame submissions. The display's last moving-average FPS stays stale on a hang.
    // No formatting, allocation, locks or disk writes occur on the presentation path.
    inline std::atomic<std::uint64_t> presentedFrames{};
}
