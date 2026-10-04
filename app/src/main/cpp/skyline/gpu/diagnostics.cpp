// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <deque>
#include <mutex>
#include <fmt/format.h>
#include <fcntl.h>
#include <unistd.h>
#include "diagnostics.h"

namespace skyline::gpu::diagnostics {
    namespace {
        constexpr size_t MaxEvents{400}; //!< How many of the most recent events are kept

        std::mutex mutex;
        std::deque<std::string> events;
        std::string path;
        bool reported{};
        const auto startTime{std::chrono::steady_clock::now()};
    }

    void Initialize(std::string reportPath) {
        std::scoped_lock lock{mutex};
        path = std::move(reportPath);
    }

    void Record(std::string_view category, std::string_view message) {
        auto elapsedMs{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count()};
        std::scoped_lock lock{mutex};
        if (events.size() >= MaxEvents)
            events.pop_front();
        events.push_back(fmt::format("[{:>8}ms] {:<9} {}", elapsedMs, category, message));
    }

    void DumpFaultReport(std::string_view reason) {
        std::scoped_lock lock{mutex};
        if (reported || path.empty())
            return;
        reported = true;

        std::string report{fmt::format("===== GPU fault: {} =====\nThe {} most recent GPU events, oldest first:\n", reason, events.size())};
        for (const auto &event : events) {
            report += event;
            report += '\n';
        }
        report += "===== end of report =====\n\n";

        int fd{open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
        if (fd >= 0) {
            [[maybe_unused]] auto written{write(fd, report.data(), report.size())};
            close(fd);
        }
    }
}
