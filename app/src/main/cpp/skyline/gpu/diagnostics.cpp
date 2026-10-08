// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cerrno>
#include <deque>
#include <mutex>
#include <fmt/format.h>
#include <fcntl.h>
#include <unistd.h>
#include <common/diagnostic_session.h>
#include "diagnostics.h"

namespace skyline::gpu::diagnostics {
    namespace {
        constexpr size_t MaxEvents{400}; //!< How many of the most recent events are kept

        std::mutex mutex;
        std::deque<std::string> events;
        std::string path;
        bool reported{};
        auto startTime{std::chrono::steady_clock::now()};
    }

    void Initialize(std::string reportPath) noexcept {
        try {
            std::scoped_lock lock{mutex};
            path = skyline::diagnostics::OutputPath("gpu_fault.log", std::move(reportPath));
            events.clear();
            reported = false;
            startTime = std::chrono::steady_clock::now();
        } catch (...) {
        }
    }

    void Record(std::string_view category, std::string_view message) noexcept {
        try {
            std::scoped_lock lock{mutex};
            auto elapsedMs{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count()};
            if (events.size() >= MaxEvents)
                events.pop_front();
            events.push_back(fmt::format("[{:>8}ms] {:<9} {}", elapsedMs, category, message));
        } catch (...) {
            // In particular, memory pressure while reporting a GPU failure is non-fatal.
        }
    }

    void DumpFaultReport(std::string_view reason) noexcept {
        try {
            std::scoped_lock lock{mutex};
            if (reported || path.empty())
                return;

            std::string report{fmt::format("===== GPU fault: {} =====\nThe {} most recent GPU events, oldest first:\n", reason, events.size())};
            for (const auto &event : events) {
                report += event;
                report += '\n';
            }
            report += "===== end of report =====\n\n";

            int fd{open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)};
            if (fd >= 0) {
                size_t offset{};
                while (offset < report.size()) {
                    auto count{write(fd, report.data() + offset, report.size() - offset)};
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        break;
                    offset += static_cast<size_t>(count);
                }
                reported = offset == report.size();
                close(fd);
            }
        } catch (...) {
        }
    }
}
