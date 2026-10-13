// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace skyline::kernel::scheduler_detail {
    // Times are CNTVCT ticks, never absolute uptime in a duration average.
    constexpr std::uint64_t UpdateAverage(std::uint64_t previous, std::uint64_t start, std::uint64_t now) {
        if (!start || now < start)
            return previous;
        const auto elapsed{now - start};
        // floor((previous + 3 * elapsed) / 4), without overflowing either sum.
        return previous / 4 + 3 * (elapsed / 4) + (previous % 4 + 3 * (elapsed % 4)) / 4;
    }

    constexpr std::uint64_t RemainingTimeslice(std::uint64_t average, std::uint64_t start, std::uint64_t now) {
        const auto elapsed{start && now >= start ? now - start : 0};
        if (average)
            return average > elapsed ? average - elapsed : 1;
        return elapsed ? elapsed : 1;
    }

    constexpr std::uint64_t AddTimeslices(std::uint64_t total, std::uint64_t duration) {
        constexpr auto maximum{std::numeric_limits<std::uint64_t>::max()};
        return duration > maximum - total ? maximum : total + duration;
    }

    // An idle core does not notify waiters on other cores. Keep checking for
    // migration opportunities, without an unbounded exponential delay.
    constexpr std::chrono::milliseconds NextLoadBalanceDelay(std::chrono::milliseconds delay) {
        constexpr std::chrono::milliseconds maximum{160};
        return delay >= maximum / 2 ? maximum : delay * 2;
    }
}
