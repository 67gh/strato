// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
namespace skyline::diagnostics {
void InitializeCrashCapture() noexcept;
void SnapshotModules() noexcept;
void CloseCrashCapture() noexcept;
// Preopened descriptor, one fixed record, no allocation, locks, stack walking or formatting.
void RecordRawCrash(int signal, std::uint64_t fault, std::uint64_t pc, std::uint64_t lr,
                    std::uint64_t sp, std::uint64_t fp, bool guest) noexcept;
}
