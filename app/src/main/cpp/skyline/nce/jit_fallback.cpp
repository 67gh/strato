// SPDX-License-Identifier: MPL-2.0

#include <fcntl.h>
#include <unistd.h>
#include <unordered_set>
#include <asm/sigcontext.h>
#include <common/signal.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include <os.h>
#include "jit_fallback.h"

#ifdef STRATO_JIT_FALLBACK
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#endif

namespace skyline::nce {
    namespace {
        const DeviceState *deviceState{};
        int logFd{-1};
        std::mutex logMutex;
        std::unordered_set<u64> seen; //!< Failures already written, so a hot loop doesn't flood the file

        /**
         * @brief Describes the instruction class, system register accesses are the most common thing NCE misses so they're decoded fully
         */
        std::string Classify(u32 insn) {
            if ((insn & 0xFFF00000) == 0xD5300000 || (insn & 0xFFF00000) == 0xD5100000) {
                u32 reg{(insn >> 5) & 0x7FFF};
                return fmt::format("{} system register op0={} op1={} CRn={} CRm={} op2={}", (insn & 0x00200000) ? "MRS" : "MSR", 2 + ((reg >> 14) & 1), (reg >> 11) & 7, (reg >> 7) & 0xF, (reg >> 3) & 0xF, reg & 7);
            }
            if ((insn & 0xFFF80000) == 0xD5080000)
                return "SYS/DC/IC/TLBI cache or TLB maintenance";
            switch ((insn >> 25) & 0xF) {
                case 0x0: return "reserved/unallocated (op0=0)";
                case 0x1: return "unallocated (op0=1)";
                case 0x2: return "SVE (op0=2)";
                case 0x3: return "unallocated (op0=3)";
                case 0x8: case 0x9: return "data processing immediate";
                case 0xA: case 0xB: return "branch/exception/system";
                case 0x4: case 0x6: case 0xC: case 0xE: return "load/store";
                case 0x5: case 0xD: return "data processing register";
                default: return "SIMD/FP";
            }
        }

        void WriteLine(const std::string &line) {
            if (logFd >= 0)
                [[maybe_unused]] auto result{write(logFd, line.data(), line.size())};
        }

        void Log(int signal, siginfo *info, ucontext *ctx, JitFallback::Outcome outcome, std::string_view detail) {
            auto &mctx{ctx->uc_mcontext};
            u32 insn{};
            if (mctx.pc)
                std::memcpy(&insn, reinterpret_cast<void *>(mctx.pc), sizeof(insn)); // Guest .text is readable, the instruction at PC is the one that failed

            u64 key{mctx.pc ^ (static_cast<u64>(insn) << 32) ^ (static_cast<u64>(outcome) << 61) ^ static_cast<u64>(signal)};
            std::scoped_lock lock{logMutex};
            if (!seen.insert(key).second)
                return;

            static constexpr std::array<std::string_view, 4> OutcomeNames{"recovered_by_jit", "jit_unsupported", "jit_memory_fault", "not_attempted"};
            signal::StackFrame topFrame{.lr = reinterpret_cast<void *>(mctx.pc), .next = reinterpret_cast<signal::StackFrame *>(mctx.regs[29])};
            std::string where{deviceState && deviceState->loader ? deviceState->loader->GetStackTrace(&topFrame) : ""};
            std::replace(where.begin(), where.end(), '\n', ' ');

            WriteLine(fmt::format(R"({{"type":"failure","pc":"0x{:X}","insn":"0x{:08X}","class":"{}","signal":"{}","si_code":{},"fault_addr":"0x{:X}","outcome":"{}","detail":"{}","where":"{}"}})" "\n",
                                  mctx.pc, insn, Classify(insn), strsignal(signal), info->si_code, mctx.fault_address, OutcomeNames[static_cast<size_t>(outcome)], detail, where));
        }

#ifdef STRATO_JIT_FALLBACK
        struct StepCallbacks final : Dynarmic::A64::UserCallbacks {
            Dynarmic::A64::Jit *jit{};
            JitFallback::Outcome outcome{JitFallback::Outcome::Recovered};
            std::string detail;

            bool Valid(u64 address, size_t size) const {
                return deviceState->process->memory.AddressSpaceContains(span<u8>{reinterpret_cast<u8 *>(address), size});
            }

            template<typename T>
            T Read(u64 address) {
                if (!Valid(address, sizeof(T))) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest read at 0x{:X}", address));
                    return {};
                }
                return *reinterpret_cast<T *>(address);
            }

            template<typename T>
            void Write(u64 address, T value) {
                if (!Valid(address, sizeof(T)))
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest write at 0x{:X}", address));
                else
                    *reinterpret_cast<T *>(address) = value;
            }

            template<typename T>
            bool WriteExclusive(u64 address, T value, T expected) {
                if (!Valid(address, sizeof(T))) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest exclusive write at 0x{:X}", address));
                    return false;
                }
                return __atomic_compare_exchange_n(reinterpret_cast<T *>(address), &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            }

            void Fail(JitFallback::Outcome reason, std::string text) {
                if (outcome == JitFallback::Outcome::Recovered) {
                    outcome = reason;
                    detail = std::move(text);
                }
                jit->HaltExecution();
            }

            std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override { return Valid(vaddr, 4) ? std::optional{*reinterpret_cast<u32 *>(vaddr)} : std::nullopt; }
            u8 MemoryRead8(u64 vaddr) override { return Read<u8>(vaddr); }
            u16 MemoryRead16(u64 vaddr) override { return Read<u16>(vaddr); }
            u32 MemoryRead32(u64 vaddr) override { return Read<u32>(vaddr); }
            u64 MemoryRead64(u64 vaddr) override { return Read<u64>(vaddr); }
            Dynarmic::A64::Vector MemoryRead128(u64 vaddr) override { return Read<Dynarmic::A64::Vector>(vaddr); }
            void MemoryWrite8(u64 vaddr, u8 value) override { Write(vaddr, value); }
            void MemoryWrite16(u64 vaddr, u16 value) override { Write(vaddr, value); }
            void MemoryWrite32(u64 vaddr, u32 value) override { Write(vaddr, value); }
            void MemoryWrite64(u64 vaddr, u64 value) override { Write(vaddr, value); }
            void MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) override { Write(vaddr, value); }
            bool MemoryWriteExclusive8(u64 vaddr, u8 value, u8 expected) override { return WriteExclusive(vaddr, value, expected); }
            bool MemoryWriteExclusive16(u64 vaddr, u16 value, u16 expected) override { return WriteExclusive(vaddr, value, expected); }
            bool MemoryWriteExclusive32(u64 vaddr, u32 value, u32 expected) override { return WriteExclusive(vaddr, value, expected); }
            bool MemoryWriteExclusive64(u64 vaddr, u64 value, u64 expected) override { return WriteExclusive(vaddr, value, expected); }
            bool MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) override {
                Fail(JitFallback::Outcome::JitUnsupported, "128-bit exclusive store");
                return false;
            }

            void InterpreterFallback(u64 pc, size_t count) override { Fail(JitFallback::Outcome::JitUnsupported, fmt::format("Dynarmic has no implementation for the instruction at 0x{:X}", pc)); }
            void CallSVC(u32 swi) override { Fail(JitFallback::Outcome::JitUnsupported, fmt::format("unpatched SVC #{}", swi)); } // SVCs are patched at load time, so one reaching here means the patcher missed it
            void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override { Fail(JitFallback::Outcome::JitUnsupported, fmt::format("Dynarmic exception {} at 0x{:X}", static_cast<int>(exception), pc)); }
            void AddTicks(u64 ticks) override {}
            u64 GetTicksRemaining() override { return 1; }
            u64 GetCNTPCT() override {
                u64 value;
                asm volatile("MRS %0, CNTVCT_EL0" : "=r"(value));
                return value;
            }
        };

        struct StepJob {
            ucontext *ctx;
            ThreadContext *threadCtx;
            JitFallback::Outcome outcome;
            std::string detail;
        };

        fpsimd_context *FindFpsimd(mcontext_t &mctx) {
            for (auto *head{reinterpret_cast<_aarch64_ctx *>(mctx.__reserved)}; head->magic; head = reinterpret_cast<_aarch64_ctx *>(reinterpret_cast<u8 *>(head) + head->size))
                if (head->magic == FPSIMD_MAGIC)
                    return reinterpret_cast<fpsimd_context *>(head);
            return nullptr;
        }

        /**
         * @brief Executes the instruction at the signal context's PC with Dynarmic and writes the state back, must run on the host stack
         */
        void StepOnHostStack(void *argument) {
            auto &job{*reinterpret_cast<StepJob *>(argument)};
            auto &mctx{job.ctx->uc_mcontext};
            auto *fp{FindFpsimd(mctx)};
            if (!fp) {
                job.outcome = JitFallback::Outcome::JitUnsupported;
                job.detail = "no FPSIMD context in signal frame";
                return;
            }

            // One JIT per guest thread, its block cache then makes repeated failures of the same instruction cheap
            thread_local StepCallbacks callbacks;
            thread_local std::unique_ptr<Dynarmic::A64::Jit> jit;
            if (!jit) {
                Dynarmic::A64::UserConfig config{};
                config.callbacks = &callbacks;
                config.tpidrro_el0 = reinterpret_cast<u64 *>(&job.threadCtx->tpidrroEl0);
                config.tpidr_el0 = reinterpret_cast<u64 *>(&job.threadCtx->tpidrEl0);
                config.define_unpredictable_behaviour = true;
                jit = std::make_unique<Dynarmic::A64::Jit>(config);
                callbacks.jit = jit.get();
            }
            callbacks.outcome = JitFallback::Outcome::Recovered;
            callbacks.detail.clear();

            for (size_t i{}; i < 31; i++)
                jit->SetRegister(i, mctx.regs[i]);
            jit->SetSP(mctx.sp);
            jit->SetPC(mctx.pc);
            jit->SetPstate(static_cast<u32>(mctx.pstate) & 0xF0000000);
            jit->SetFpcr(fp->fpcr);
            jit->SetFpsr(fp->fpsr);
            Dynarmic::A64::Jit::Vector vectors;
            for (size_t i{}; i < 32; i++)
                std::memcpy(&vectors[i], &fp->vregs[i], sizeof(vectors[i]));
            jit->SetVectors(vectors);

            jit->InvalidateCacheRange(mctx.pc, 4); // Guest code could have been rewritten since it was last translated
            jit->Step();

            job.outcome = callbacks.outcome;
            job.detail = callbacks.detail;
            if (job.outcome != JitFallback::Outcome::Recovered)
                return; // Leave the signal context untouched so the crash handler reports the original state

            auto regs{jit->GetRegisters()};
            for (size_t i{}; i < 31; i++)
                mctx.regs[i] = regs[i];
            mctx.sp = jit->GetSP();
            mctx.pc = jit->GetPC();
            mctx.pstate = (mctx.pstate & ~0xF0000000ULL) | (jit->GetPstate() & 0xF0000000);
            fp->fpcr = jit->GetFpcr();
            fp->fpsr = jit->GetFpsr();
            vectors = jit->GetVectors();
            for (size_t i{}; i < 32; i++)
                std::memcpy(&fp->vregs[i], &vectors[i], sizeof(vectors[i]));
        }

        /**
         * @brief Calls fn(arg) with SP switched to the given stack, the signal stack is too small for Dynarmic's translator
         */
        void CallOnStack(void (*fn)(void *), void *arg, u8 *stackTop) {
            asm volatile(
            "MOV X19, SP\n\t"
            "MOV SP, %1\n\t"
            "MOV X0, %2\n\t"
            "BLR %0\n\t"
            "MOV SP, X19\n\t"
            :
            : "r"(fn), "r"(stackTop), "r"(arg)
            : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x19", "x30",
              "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
              "memory", "cc");
        }
#endif
    }

    void JitFallback::Initialize(const DeviceState &state, std::string logPath, std::string gameName) {
        deviceState = &state;
        logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (logFd < 0)
            return;

        std::replace(gameName.begin(), gameName.end(), '"', '\'');
        WriteLine(fmt::format(R"({{"type":"session","game":"{}","jit_compiled":{}}})" "\n", gameName,
#ifdef STRATO_JIT_FALLBACK
                              "true"
#else
                              "false"
#endif
        ));
    }

    bool JitFallback::HandleFault(int signal, siginfo *info, ucontext *ctx, ThreadContext &threadCtx) {
        Outcome outcome{Outcome::NotAttempted};
        std::string detail{signal == SIGILL ? "JIT not compiled in (build with -DSTRATO_JIT_FALLBACK=ON)" : "signal is not recoverable by instruction emulation"};

#ifdef STRATO_JIT_FALLBACK
        if (signal == SIGILL && info->si_code != SI_USER) {
            StepJob job{ctx, &threadCtx, Outcome::NotAttempted, {}};
            CallOnStack(&StepOnHostStack, &job, threadCtx.hostSp);
            outcome = job.outcome;
            detail = job.detail.empty() ? "single-stepped with Dynarmic" : job.detail;
        }
#endif

        Log(signal, info, ctx, outcome, detail);
        return outcome == Outcome::Recovered;
    }
}
