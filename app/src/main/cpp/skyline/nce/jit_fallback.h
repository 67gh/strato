// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <common.h>
#include "guest.h"

namespace skyline::nce {
    /**
     * @brief Fallback for guest instructions that NCE cannot run natively
     * @details When a guest thread faults (typically SIGILL on an instruction the host CPU lacks or traps), the single faulting
     * instruction is executed by Dynarmic (if built with STRATO_JIT_FALLBACK), the resulting register state is written back into the
     * signal context and the guest resumes natively. A bounded JSON-lines diagnostic keeps the original fault separate from the
     * resulting JIT state. Recovery alone does not establish a missing host instruction or prove equivalent NCE/JIT semantics.
     * @note The fallback still allocates and accesses guest data memory from a signal path; these diagnostics do not make that path
     * async-signal-safe or provide memory rollback. Only instruction inspection uses a fault-contained read.
     */
    class JitFallback {
      public:
        enum class Outcome {
            Recovered, //!< The JIT executed the instruction and the guest resumed
            JitUnsupported, //!< The JIT could not execute the instruction either
            MemoryFault, //!< The JIT hit an invalid guest memory access
            NotAttempted, //!< Signal isn't recoverable by the JIT, or the JIT isn't compiled in
        };

        /**
         * @param logPath The JSON-lines file that failures are appended to
         * @param gameName Written to the file header so logs from different games can be told apart
         */
        static void Initialize(const DeviceState &state, std::string logPath, std::string gameName);

        /**
         * @brief Writes final occurrence counts and closes the diagnostic descriptors
         * @note Call from the host after guest threads have stopped, before destroying the logger. A hard kill may lose counts since
         * the last checkpoint; the analyzer reports those counts as lower bounds when no session_end record is present.
         */
        static void Flush() noexcept;

        /**
         * @brief Called from NCE::SignalHandler for a fault in guest code; captures it before any JIT changes to the signal context
         * @return If the guest can be resumed (the signal context was updated), otherwise the caller must treat the fault as fatal
         */
        static bool HandleFault(int signal, siginfo *info, ucontext *ctx, ThreadContext &threadCtx);
    };
}
