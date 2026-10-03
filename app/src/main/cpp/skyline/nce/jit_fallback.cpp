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

        struct SysReg {
            u8 op0, op1, crn, crm, op2;
            const char *name;
        };

        constexpr std::array<SysReg, 26> SysRegs{{
            {3, 3, 13, 0, 2, "TPIDR_EL0"}, {3, 3, 13, 0, 3, "TPIDRRO_EL0"}, {3, 3, 14, 0, 0, "CNTFRQ_EL0"}, {3, 3, 14, 0, 1, "CNTPCT_EL0"}, {3, 3, 14, 0, 2, "CNTVCT_EL0"},
            {3, 3, 0, 0, 1, "CTR_EL0"}, {3, 3, 0, 0, 7, "DCZID_EL0"}, {3, 3, 4, 4, 0, "FPCR"}, {3, 3, 4, 4, 1, "FPSR"}, {3, 3, 4, 2, 0, "NZCV"}, {3, 3, 4, 2, 1, "DAIF"},
            {3, 0, 0, 0, 0, "MIDR_EL1"}, {3, 0, 0, 0, 5, "MPIDR_EL1"}, {3, 0, 0, 0, 6, "REVIDR_EL1"}, {3, 0, 0, 4, 0, "ID_AA64PFR0_EL1"}, {3, 0, 0, 4, 1, "ID_AA64PFR1_EL1"},
            {3, 0, 0, 5, 0, "ID_AA64DFR0_EL1"}, {3, 0, 0, 6, 0, "ID_AA64ISAR0_EL1"}, {3, 0, 0, 6, 1, "ID_AA64ISAR1_EL1"}, {3, 0, 0, 7, 0, "ID_AA64MMFR0_EL1"}, {3, 0, 0, 7, 1, "ID_AA64MMFR1_EL1"},
            {3, 3, 9, 13, 0, "PMCCNTR_EL0"}, {3, 3, 9, 12, 0, "PMCR_EL0"}, {3, 3, 9, 12, 3, "PMCNTENSET_EL0"}, {3, 3, 9, 14, 0, "PMUSERENR_EL0"}, {3, 3, 14, 3, 1, "CNTV_CTL_EL0"},
        }};

        /**
         * @return A short human readable description of the instruction, system register accesses and SYS/hint/barrier instructions are fully named
         */
        std::string Decode(u32 insn) {
            if ((insn & 0xFFD00000) == 0xD5100000) { // MRS/MSR (register)
                u8 op0{static_cast<u8>(2 + ((insn >> 19) & 1))}, op1{static_cast<u8>((insn >> 16) & 7)}, crn{static_cast<u8>((insn >> 12) & 0xF)}, crm{static_cast<u8>((insn >> 8) & 0xF)}, op2{static_cast<u8>((insn >> 5) & 7)};
                std::string name{fmt::format("S{}_{}_C{}_C{}_{}", op0, op1, crn, crm, op2)};
                for (auto &reg : SysRegs)
                    if (reg.op0 == op0 && reg.op1 == op1 && reg.crn == crn && reg.crm == crm && reg.op2 == op2)
                        name = reg.name;
                return (insn & 0x00200000) ? fmt::format("MRS X{}, {}", insn & 0x1F, name) : fmt::format("MSR {}, X{}", name, insn & 0x1F);
            }
            if ((insn & 0xFFF80000) == 0xD5080000) { // SYS (DC/IC/TLBI/AT)
                u8 op1{static_cast<u8>((insn >> 16) & 7)}, crn{static_cast<u8>((insn >> 12) & 0xF)}, crm{static_cast<u8>((insn >> 8) & 0xF)}, op2{static_cast<u8>((insn >> 5) & 7)};
                const char *name{nullptr};
                if (op1 == 3 && crn == 7 && crm == 4 && op2 == 1) name = "DC ZVA";
                else if (op1 == 3 && crn == 7 && crm == 10 && op2 == 1) name = "DC CVAC";
                else if (op1 == 3 && crn == 7 && crm == 11 && op2 == 1) name = "DC CVAU";
                else if (op1 == 3 && crn == 7 && crm == 14 && op2 == 1) name = "DC CIVAC";
                else if (op1 == 3 && crn == 7 && crm == 5 && op2 == 1) name = "IC IVAU";
                else if (op1 == 0 && crn == 7 && crm == 6 && op2 == 1) name = "DC IVAC";
                else if (op1 == 0 && crn == 7 && crm == 6 && op2 == 2) name = "DC ISW";
                return fmt::format("SYS {} (op1={} CRn={} CRm={} op2={}) X{}", name ? name : "?", op1, crn, crm, op2, insn & 0x1F);
            }
            if ((insn & 0xFFFFF01F) == 0xD503301F) {
                switch ((insn >> 5) & 7) {
                    case 2: return "CLREX";
                    case 4: return fmt::format("DSB #{}", (insn >> 8) & 0xF);
                    case 5: return fmt::format("DMB #{}", (insn >> 8) & 0xF);
                    case 6: return fmt::format("ISB #{}", (insn >> 8) & 0xF);
                }
            }
            if ((insn & 0xFFFFF01F) == 0xD503201F) {
                u32 hint{(insn >> 5) & 0x7F};
                switch (hint) {
                    case 0: return "NOP";
                    case 1: return "YIELD";
                    case 2: return "WFE";
                    case 3: return "WFI";
                    case 4: return "SEV";
                    case 5: return "SEVL";
                    case 25: return "PACIASP (pointer authentication)";
                    case 29: return "AUTIASP (pointer authentication)";
                    case 32: case 34: case 36: case 38: return "BTI (branch target identification)";
                    default: return fmt::format("HINT #{}", hint);
                }
            }
            if ((insn & 0xFFE0001F) == 0xD4000001) return fmt::format("SVC #{}", (insn >> 5) & 0xFFFF);
            if ((insn & 0xFFE0001F) == 0xD4200000) return fmt::format("BRK #{}", (insn >> 5) & 0xFFFF);
            if ((insn & 0xFFE0001F) == 0xD4400000) return fmt::format("HLT #{}", (insn >> 5) & 0xFFFF);
            if ((insn & 0x3B200C00) == 0x38200000) return "LSE atomic memory operation (ARMv8.1 LDADD/SWP/etc.)";
            if ((insn & 0x3FA07C00) == 0x08A07C00) return "LSE compare-and-swap (ARMv8.1 CAS)";
            if ((insn & 0x9F000000) == 0x04000000) return "SVE instruction";
            if ((insn & 0xFFFFFC00) == 0xD65F0800 || (insn & 0xFFFFFC00) == 0xD61F0800) return "pointer authentication branch/return";
            return "unknown (decode by hand)";
        }

        std::string Classify(u32 insn) {
            if ((insn & 0xFFD00000) == 0xD5100000)
                return "system register access";
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

        std::string JsonEscape(std::string_view in) {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (c == '\n') out += "\\n";
                else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
            }
            return out;
        }

        void WriteLine(const std::string &line) {
            if (logFd >= 0)
                [[maybe_unused]] auto result{write(logFd, line.data(), line.size())};
        }

        void Log(int signal, siginfo *info, ucontext *ctx, JitFallback::Outcome outcome, std::string_view detail, std::string_view trace) {
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

            WriteLine(fmt::format(R"({{"type":"failure","pc":"0x{:X}","insn":"0x{:08X}","decoded":"{}","class":"{}","signal":"{}","si_code":{},"fault_addr":"0x{:X}","outcome":"{}","detail":"{}","where":"{}"{}}})" "\n",
                                  mctx.pc, insn, JsonEscape(Decode(insn)), Classify(insn), strsignal(signal), info->si_code, mctx.fault_address, OutcomeNames[static_cast<size_t>(outcome)], JsonEscape(detail), JsonEscape(where), trace));
        }

#ifdef STRATO_JIT_FALLBACK
        struct StepCallbacks final : Dynarmic::A64::UserCallbacks {
            Dynarmic::A64::Jit *jit{};
            JitFallback::Outcome outcome{JitFallback::Outcome::Recovered};
            std::string detail;
            std::string accesses; //!< JSON array body of the guest memory accesses the instruction made, used to work out its semantics
            size_t accessCount{};

            template<typename T>
            static std::string Hex(T value) {
                if constexpr (sizeof(T) == 16) {
                    u64 words[2];
                    std::memcpy(words, &value, sizeof(words));
                    return fmt::format("0x{:016X}{:016X}", words[1], words[0]);
                } else {
                    return fmt::format("0x{:X}", static_cast<u64>(value));
                }
            }

            template<typename T>
            void Record(char direction, u64 address, T value) {
                if (accessCount++ < 8)
                    accesses += fmt::format("{}\"{}{}@0x{:X}={}\"", accesses.empty() ? "" : ",", direction, sizeof(T) * 8, address, Hex(value));
            }

            bool Valid(u64 address, size_t size) const {
                return deviceState->process->memory.AddressSpaceContains(span<u8>{reinterpret_cast<u8 *>(address), size});
            }

            template<typename T>
            T Read(u64 address) {
                if (!Valid(address, sizeof(T))) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest read at 0x{:X}", address));
                    return {};
                }
                T value{*reinterpret_cast<T *>(address)};
                Record('R', address, value);
                return value;
            }

            template<typename T>
            void Write(u64 address, T value) {
                if (!Valid(address, sizeof(T)))
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest write at 0x{:X}", address));
                else {
                    *reinterpret_cast<T *>(address) = value;
                    Record('W', address, value);
                }
            }

            template<typename T>
            bool WriteExclusive(u64 address, T value, T expected) {
                if (!Valid(address, sizeof(T))) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest exclusive write at 0x{:X}", address));
                    return false;
                }
                bool success{__atomic_compare_exchange_n(reinterpret_cast<T *>(address), &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)};
                if (success)
                    Record('X', address, value);
                return success;
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
            std::string trace; //!< Extra JSON fields (leading comma) describing what the JIT did, appended to the log line
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
            callbacks.accesses.clear();
            callbacks.accessCount = 0;

            u32 insn{};
            std::memcpy(&insn, reinterpret_cast<void *>(mctx.pc), sizeof(insn));
            [[maybe_unused]] bool dumpDisassembly{};
#ifdef STRATO_JIT_DUMP_DISASSEMBLY
            {
                static std::mutex dumpMutex;
                static std::unordered_set<u32> dumped; // Only dump the first time an instruction word is seen, it's expensive
                std::scoped_lock lock{dumpMutex};
                dumpDisassembly = dumped.insert(insn).second;
            }
            if (dumpDisassembly)
                jit->ClearCache(); // So the cache then only holds the block for this instruction
#endif

            std::array<u64, 31> regsBefore;
            for (size_t i{}; i < 31; i++)
                regsBefore[i] = mctx.regs[i];
            u64 spBefore{mctx.sp}, pcBefore{mctx.pc};
            u32 nzcvBefore{static_cast<u32>(mctx.pstate) & 0xF0000000};
            auto *vregsBefore{fp->vregs};
            std::array<std::array<u8, 16>, 32> vecBefore;
            std::memcpy(vecBefore.data(), vregsBefore, sizeof(vecBefore));

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

            {
                // Describe what the instruction did, this is what's needed to re-implement it natively
                auto after{jit->GetRegisters()};
                std::string changed;
                auto add = [&](std::string_view name, std::string before, std::string afterValue) {
                    changed += fmt::format("{}\"{}\":[\"{}\",\"{}\"]", changed.empty() ? "" : ",", name, before, afterValue);
                };
                for (size_t i{}; i < 31; i++)
                    if (regsBefore[i] != after[i])
                        add(fmt::format("x{}", i), fmt::format("0x{:X}", regsBefore[i]), fmt::format("0x{:X}", after[i]));
                if (spBefore != jit->GetSP())
                    add("sp", fmt::format("0x{:X}", spBefore), fmt::format("0x{:X}", jit->GetSP()));
                if (nzcvBefore != (jit->GetPstate() & 0xF0000000))
                    add("nzcv", fmt::format("0x{:X}", nzcvBefore >> 28), fmt::format("0x{:X}", (jit->GetPstate() & 0xF0000000) >> 28));
                auto vectorsAfter{jit->GetVectors()};
                for (size_t i{}; i < 32; i++) {
                    if (std::memcmp(vecBefore[i].data(), &vectorsAfter[i], 16) != 0) {
                        u64 b[2], a[2];
                        std::memcpy(b, vecBefore[i].data(), 16);
                        std::memcpy(a, &vectorsAfter[i], 16);
                        add(fmt::format("v{}", i), fmt::format("0x{:016X}{:016X}", b[1], b[0]), fmt::format("0x{:016X}{:016X}", a[1], a[0]));
                    }
                }

                std::string disassembly;
#ifdef STRATO_JIT_DUMP_DISASSEMBLY
                if (dumpDisassembly) {
                    // NOTE: older Dynarmic versions only have DumpDisassembly() (prints to stdout), newer ones return a string from Disassemble()
                    disassembly = fmt::format(R"(,"jit_disassembly":"{}")", JsonEscape(jit->Disassemble().substr(0, 4096)));
                }
#endif
                job.trace = fmt::format(R"(,"jit":{{"next_pc":"0x{:X}","regs_changed":{{{}}},"mem":[{}]}}{})", jit->GetPC(), changed, callbacks.accesses, disassembly);
            }

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
        if (logFd < 0) {
            LOGW("NCE fallback: couldn't open '{}' for writing, failures won't be recorded", logPath);
            return;
        }
        LOGINF("NCE fallback: Dynarmic JIT {}, logging guest faults to '{}'",
#ifdef STRATO_JIT_FALLBACK
               "ENABLED",
#else
               "NOT compiled in (build with -DSTRATO_JIT_FALLBACK=ON)",
#endif
               logPath);

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
        std::string trace;
        std::string detail{signal == SIGILL ? "JIT not compiled in (build with -DSTRATO_JIT_FALLBACK=ON)" : "signal is not recoverable by instruction emulation"};

#ifdef STRATO_JIT_FALLBACK
        if (signal == SIGILL && info->si_code != SI_USER) {
            StepJob job{ctx, &threadCtx, Outcome::NotAttempted, {}};
            CallOnStack(&StepOnHostStack, &job, threadCtx.hostSp);
            outcome = job.outcome;
            detail = job.detail.empty() ? "single-stepped with Dynarmic" : job.detail;
            trace = std::move(job.trace);
        }
#endif

        Log(signal, info, ctx, outcome, detail, trace);
        return outcome == Outcome::Recovered;
    }
}
