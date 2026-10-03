// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <common.h>
#include "guest.h"

namespace skyline::nce {
    /**
     * @brief Fallback for guest instructions that NCE cannot run natively
     * @details When a guest thread faults (typically SIGILL on an instruction the host CPU lacks or traps), the single faulting
     * instruction is executed by Dynarmic (if built with STRATO_JIT_FALLBACK), the resulting register state is written back into the
     * signal context and the guest resumes natively right after it. Every distinct failure is appended to a JSON-lines file with the
     * reason, so the instructions NCE is missing can be collected and later handled natively in NCE::PatchCode.
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
         * @brief Called from NCE::SignalHandler for a fault in guest code, always logs the failure
         * @return If the guest can be resumed (the signal context was updated), otherwise the caller must treat the fault as fatal
         */
        static bool HandleFault(int signal, siginfo *info, ucontext *ctx, ThreadContext &threadCtx);
    };
}
