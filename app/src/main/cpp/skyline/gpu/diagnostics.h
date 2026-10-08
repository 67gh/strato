// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

namespace skyline::gpu::diagnostics {
    /**
     * @brief Sets the file that fault reports are appended to, must be called before anything else
     */
    void Initialize(std::string reportPath) noexcept;

    /**
     * @brief Remembers an event in a ring buffer of the most recent ones (nothing is written to disk)
     * @param category A short tag such as "vulkan" or "pipeline"
     */
    void Record(std::string_view category, std::string_view message) noexcept;

    /**
     * @brief Writes the recent events to the report file, called when the GPU faults so the events leading up to it are kept
     * @note Only the first report of a run is written (the faults that follow are consequences of the first one)
     */
    void DumpFaultReport(std::string_view reason) noexcept;
}
