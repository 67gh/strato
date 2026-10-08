// SPDX-License-Identifier: MPL-2.0

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <limits>
#include <map>
#include <tuple>
#include <sys/syscall.h>
#include <time.h>
#include <unordered_set>
#include <asm/sigcontext.h>
#include <common/signal.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include <kernel/types/KThread.h>
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
        int memoryFd{-1};
        int memoryOpenError{EBADF};
        std::mutex logMutex;
        constexpr size_t MaxTrackedFailures{1024};
        constexpr size_t MaxLogBytes{8 * 1024 * 1024};
        constexpr size_t FinalCountsReserve{1024 * 1024};
        // Full tuple equality, not a lossy XOR: distinct faults must not silently merge.
        using FailureKey = std::tuple<u64, u32, int, int, int, JitFallback::Outcome>;
        struct FailureCount {
            u64 id;
            u64 count;
            u64 reported;
            u64 lastMonotonicNs;
        };
        std::map<FailureKey, FailureCount> seen;
        std::vector<loader::Loader::ExecutableRange> executableRanges; // Copied before guest threads start; no symbol/stack reads in Log.
        std::string sessionId;
        u64 sessionSerial{};
        std::atomic<u64> totalFaults{};
        std::atomic<u64> contendedFaults{};
        u64 untrackedFaults{};
        size_t logBytes{};
        u64 omittedLines{};
        int writeError{};

        u64 TimestampNs(clockid_t clock) {
            timespec now{};
            if (clock_gettime(clock, &now) != 0)
                return 0; // Zero explicitly denotes an unavailable timestamp.
            return static_cast<u64>(now.tv_sec) * 1'000'000'000ULL + static_cast<u64>(now.tv_nsec);
        }

        bool ReadInstruction(u64 pc, u32 &instruction, int &error) {
            error = 0;
            if (memoryFd < 0) {
                error = memoryOpenError;
                return false;
            }
            if ((pc & 3) || pc > static_cast<u64>(std::numeric_limits<off_t>::max()) - sizeof(instruction)) {
                error = EINVAL;
                return false;
            }
            ssize_t result;
            do {
                result = pread(memoryFd, &instruction, sizeof(instruction), static_cast<off_t>(pc));
            } while (result < 0 && errno == EINTR);
            if (result != static_cast<ssize_t>(sizeof(instruction))) {
                error = result < 0 ? errno : EIO;
                instruction = 0;
                return false;
            }
            return true;
        }

        fpsimd_context *FindFpsimd(mcontext_t &mctx) {
            auto *cursor{reinterpret_cast<u8 *>(mctx.__reserved)};
            size_t remaining{sizeof(mctx.__reserved)};
            while (remaining >= sizeof(_aarch64_ctx)) {
                auto *head{reinterpret_cast<_aarch64_ctx *>(cursor)};
                if (!head->magic || head->size < sizeof(_aarch64_ctx) || head->size > remaining || (head->size & 15))
                    break;
                if (head->magic == FPSIMD_MAGIC)
                    return head->size >= sizeof(fpsimd_context) ? reinterpret_cast<fpsimd_context *>(head) : nullptr;
                cursor += head->size;
                remaining -= head->size;
            }
            return nullptr;
        }

        struct FaultSnapshot {
            u64 pc, sp, pstate, faultAddress;
            std::array<u64, 31> regs;
            u64 unixNs, monotonicNs;
            long hostTid;
            i64 guestThreadId;
            int signal, signalCode;
            u32 instruction{};
            int instructionReadError{};
            bool fpAvailable{};
            u32 fpcr{}, fpsr{};
        };

        FaultSnapshot CaptureFault(int signal, const siginfo &info, ucontext &ctx) {
            const auto &mctx{ctx.uc_mcontext};
            FaultSnapshot fault{
                .pc = mctx.pc, .sp = mctx.sp, .pstate = mctx.pstate,
                .faultAddress = info.si_code > 0 ? reinterpret_cast<u64>(info.si_addr) : 0,
                .unixNs = TimestampNs(CLOCK_REALTIME), .monotonicNs = TimestampNs(CLOCK_MONOTONIC),
                .hostTid = syscall(SYS_gettid),
                .guestThreadId = DeviceState::thread ? static_cast<i64>(DeviceState::thread->id) : -1,
                .signal = signal, .signalCode = info.si_code,
            };
            std::copy(std::begin(mctx.regs), std::end(mctx.regs), fault.regs.begin());
            ReadInstruction(fault.pc, fault.instruction, fault.instructionReadError);
            if (auto *fp{FindFpsimd(ctx.uc_mcontext)}) {
                fault.fpAvailable = true;
                fault.fpcr = fp->fpcr;
                fault.fpsr = fp->fpsr;
            }
            return fault;
        }

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
                else if (static_cast<unsigned char>(c) < 0x20) out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
                else out += c;
            }
            return out;
        }

        bool WriteLine(const std::string &line, bool finalCounts = false) {
            const size_t limit{finalCounts ? MaxLogBytes : MaxLogBytes - FinalCountsReserve};
            if (logFd < 0 || writeError || line.size() > limit - std::min(logBytes, limit)) {
                ++omittedLines;
                return false;
            }
            size_t offset{};
            while (offset < line.size()) {
                auto result{write(logFd, line.data() + offset, line.size() - offset)};
                if (result < 0 && errno == EINTR)
                    continue;
                if (result <= 0) {
                    writeError = result < 0 ? errno : EIO;
                    ++omittedLines;
                    return false; // Do not append more JSON to a potentially partial line.
                }
                offset += static_cast<size_t>(result);
                logBytes += static_cast<size_t>(result);
            }
            return true;
        }

        std::string_view OutcomeName(JitFallback::Outcome outcome) {
            static constexpr std::array<std::string_view, 4> Names{"recovered_by_jit", "jit_unsupported", "jit_memory_fault", "not_attempted"};
            return Names[static_cast<size_t>(outcome)];
        }

        std::string_view SignalClass(const FaultSnapshot &fault) {
            if (fault.signalCode <= 0)
                return "software_generated_signal";
            if (fault.signal == SIGILL)
                return "instruction_signal";
            if (fault.signal == SIGSEGV || fault.signal == SIGBUS)
                return "memory_signal";
            if (fault.signal == SIGFPE)
                return "arithmetic_signal";
            return "other_signal";
        }

        std::string InstructionJson(u32 instruction, int readError) {
            return readError ? "null" : fmt::format("\"0x{:08X}\"", instruction);
        }

        std::string LocationJson(u64 pc) {
            for (const auto &range : executableRanges) {
                if (pc < range.patchStart || pc >= range.programEnd)
                    continue;
                auto section{pc < range.hookStart ? "patch" : pc < range.programStart ? "hook" : "text_or_data"};
                u64 sectionStart{pc < range.hookStart ? range.patchStart : pc < range.programStart ? range.hookStart : range.programStart};
                i64 offset{pc >= range.programStart ? static_cast<i64>(pc - range.programStart) : -static_cast<i64>(range.programStart - pc)};
                return fmt::format(R"(,"module":"{}","module_offset":{},"section":"{}","section_offset":"0x{:X}")",
                                   JsonEscape(range.name), offset, section, pc - sectionStart);
            }
            return R"(,"module":null,"module_offset":null,"section":null,"section_offset":null)";
        }

        void WriteCount(const FailureKey &key, FailureCount &entry, bool finalCounts = false) {
            auto [pc, instruction, readError, signal, signalCode, outcome]{key};
            if (WriteLine(fmt::format(R"({{"type":"count","schema_version":2,"session_id":"{}","event_id":{},"count":{},"last_monotonic_ns":{},"pc":"0x{:X}","insn":{},"opcode_read_errno":{},"signal_number":{},"si_code":{},"outcome":"{}"{}}})" "\n",
                                     sessionId, entry.id, entry.count, entry.lastMonotonicNs, pc, InstructionJson(instruction, readError), readError,
                                     signal, signalCode, OutcomeName(outcome), LocationJson(pc)), finalCounts))
                entry.reported = entry.count;
        }

        void FlushLocked() {
            if (logFd >= 0) {
                for (auto &[key, entry] : seen)
                    if (entry.count != entry.reported)
                        WriteCount(key, entry, true);
                WriteLine(fmt::format(R"({{"type":"session_end","schema_version":2,"session_id":"{}","unix_ns":{},"monotonic_ns":{},"total_faults":{},"tracked_keys":{},"untracked_faults":{},"contended_faults":{},"omitted_lines":{},"write_errno":{}}})" "\n",
                                      sessionId, TimestampNs(CLOCK_REALTIME), TimestampNs(CLOCK_MONOTONIC), totalFaults.load(std::memory_order_relaxed),
                                      seen.size(), untrackedFaults, contendedFaults.load(std::memory_order_relaxed), omittedLines, writeError), true);
                if (fdatasync(logFd) != 0)
                    LOGW("NCE diagnostics: final file sync failed (errno={})", errno);
                close(logFd);
                logFd = -1;
            }
            if (memoryFd >= 0) {
                close(memoryFd);
                memoryFd = -1;
            }
        }

        void Log(const FaultSnapshot &fault, const ucontext &result, JitFallback::Outcome outcome, std::string_view detail, std::string_view trace) {
            // Avoid waiting on a concurrent/recursive logger. This does not make the complete fallback async-signal-safe.
            std::unique_lock lock{logMutex, std::try_to_lock};
            if (!lock.owns_lock()) {
                contendedFaults.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (logFd < 0)
                return;
            FailureKey key{fault.pc, fault.instruction, fault.instructionReadError, fault.signal, fault.signalCode, outcome};
            if (auto found{seen.find(key)}; found != seen.end()) {
                auto &entry{found->second};
                ++entry.count;
                entry.lastMonotonicNs = fault.monotonicNs;
                if ((entry.count & (entry.count - 1)) == 0)
                    WriteCount(key, entry); // Cumulative powers-of-two checkpoints; final counts are written by Flush.
                return;
            }
            if (seen.size() >= MaxTrackedFailures) {
                ++untrackedFaults;
                if ((untrackedFaults & (untrackedFaults - 1)) == 0)
                    WriteLine(fmt::format(R"({{"type":"overflow","schema_version":2,"session_id":"{}","untracked_faults":{}}})" "\n", sessionId, untrackedFaults));
                return;
            }
            auto &entry{seen.emplace(key, FailureCount{seen.size() + 1, 1, 0, fault.monotonicNs}).first->second};
            std::string regs;
            for (auto value : fault.regs)
                regs += fmt::format("{}\"0x{:X}\"", regs.empty() ? "" : ",", value);
            const auto &after{result.uc_mcontext};
            auto line{fmt::format(R"({{"type":"failure","schema_version":2,"session_id":"{}","event_id":{},"count":1,"unix_ns":{},"monotonic_ns":{},"host_tid":{},"guest_thread_id":{},"pc":"0x{:X}","insn":{},"opcode_read_errno":{},"decoded":"{}","class":"{}","signal_number":{},"si_code":{},"fault_addr":"0x{:X}","cause_class":"{}","cause_verified":false,"outcome":"{}","detail":"{}")",
                                  sessionId, entry.id, fault.unixNs, fault.monotonicNs, fault.hostTid, fault.guestThreadId, fault.pc,
                                  InstructionJson(fault.instruction, fault.instructionReadError), fault.instructionReadError,
                                  JsonEscape(fault.instructionReadError ? "unavailable" : Decode(fault.instruction)),
                                  fault.instructionReadError ? "unavailable" : Classify(fault.instruction), fault.signal, fault.signalCode,
                                  fault.faultAddress, SignalClass(fault), OutcomeName(outcome), JsonEscape(detail))};
            line += fmt::format(R"(,"initial_state":{{"pc":"0x{:X}","sp":"0x{:X}","pstate":"0x{:X}","x":[{}],"fp_available":{},"fpcr":"0x{:X}","fpsr":"0x{:X}"}},"resumed_state":{{"committed":{},"pc":"0x{:X}","sp":"0x{:X}","pstate":"0x{:X}"}}{}{})" "}}\n",
                                fault.pc, fault.sp, fault.pstate, regs, fault.fpAvailable ? "true" : "false", fault.fpcr, fault.fpsr,
                                outcome == JitFallback::Outcome::Recovered ? "true" : "false", after.pc, after.sp, after.pstate, LocationJson(fault.pc), trace);
            if (WriteLine(line))
                entry.reported = 1;
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

                T value{};
                std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
                Record('R', address, value);
                return value;
            }

            template<typename T>
            void Write(u64 address, T value) {
                if (!Valid(address, sizeof(T))) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest write at 0x{:X}", address));
                    return;
                }

                std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(T));
                Record('W', address, value);
            }

            template<typename T>
            bool WriteExclusive(u64 address, T value, T expected) {
                if (!Valid(address, sizeof(T))) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("invalid guest exclusive write at 0x{:X}", address));
                    return false;
                }
                if ((address & (sizeof(T) - 1)) != 0) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("unaligned guest exclusive write at 0x{:X}", address));
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

            std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
                if (!Valid(vaddr, sizeof(u32)))
                    return std::nullopt;

                u32 value{};
                int error{};
                if (!ReadInstruction(vaddr, value, error)) {
                    Fail(JitFallback::Outcome::MemoryFault, fmt::format("instruction read at 0x{:X} failed (errno={})", vaddr, error));
                    return std::nullopt;
                }
                return value;
            }
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
            const FaultSnapshot *fault;
            JitFallback::Outcome outcome;
            std::string detail;
            std::string trace; //!< Extra JSON fields (leading comma) describing what the JIT did, appended to the log line
        };

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

            // One JIT per guest thread. The faulting code range is invalidated before each step for correctness.
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

            [[maybe_unused]] u32 insn{job.fault->instruction};
            [[maybe_unused]] bool dumpDisassembly{};
#ifdef STRATO_JIT_DUMP_DISASSEMBLY
            {
                static std::mutex dumpMutex;
                static std::unordered_set<u32> dumped; // Only dump the first time an instruction word is seen, it's expensive
                std::scoped_lock lock{dumpMutex};
                dumpDisassembly = dumped.size() < MaxTrackedFailures && dumped.insert(insn).second;
            }
            if (dumpDisassembly)
                jit->ClearCache(); // So the cache then only holds the block for this instruction
#endif

            std::array<u64, 31> regsBefore;
            for (size_t i{}; i < 31; i++)
                regsBefore[i] = mctx.regs[i];
            u64 spBefore{mctx.sp};
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
            std::array<Dynarmic::A64::Vector, 32> vectors;
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
                if (job.fault->fpcr != jit->GetFpcr())
                    add("fpcr", fmt::format("0x{:X}", job.fault->fpcr), fmt::format("0x{:X}", jit->GetFpcr()));
                if (job.fault->fpsr != jit->GetFpsr())
                    add("fpsr", fmt::format("0x{:X}", job.fault->fpsr), fmt::format("0x{:X}", jit->GetFpsr()));
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
                    auto instructions{jit->Disassemble()};
                    std::string text;
                    for (const auto &instruction : instructions) {
                        if (!text.empty())
                            text += "\n";
                        text += instruction;
                        if (text.size() >= 4096) {
                            text.resize(4096);
                            break;
                        }
                    }
                    disassembly = fmt::format(R"(,"jit_disassembly":"{}")", JsonEscape(text));
                }
#endif
                job.trace = fmt::format(R"(,"jit":{{"next_pc":"0x{:X}","regs_changed":{{{}}},"mem":[{}],"mem_event_count":{},"mem_truncated":{}}}{})",
                                       jit->GetPC(), changed, callbacks.accesses, callbacks.accessCount, callbacks.accessCount > 8 ? "true" : "false", disassembly);
            }

            if (job.outcome != JitFallback::Outcome::Recovered)
                return; // Registers remain untouched. Guest memory writes made by callbacks are NOT rolled back.

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
        std::scoped_lock lock{logMutex};
        FlushLocked();
        deviceState = &state;
        executableRanges = state.loader ? state.loader->GetExecutableRanges() : std::vector<loader::Loader::ExecutableRange>{};
        seen.clear();
        totalFaults.store(0, std::memory_order_relaxed);
        contendedFaults.store(0, std::memory_order_relaxed);
        untrackedFaults = 0;
        logBytes = 0;
        omittedLines = 0;
        writeError = 0;
        sessionId = fmt::format("{}-{}-{}", getpid(), TimestampNs(CLOCK_REALTIME), ++sessionSerial);

        // Report the compiled status even if the diagnostic directory is not writable.
        LOGINF("NCE fallback: Dynarmic JIT {}; diagnostic path '{}'",
#ifdef STRATO_JIT_FALLBACK
               "ENABLED",
#else
               "NOT compiled in (build with -DSTRATO_JIT_FALLBACK=ON)",
#endif
               logPath);

        memoryFd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        memoryOpenError = memoryFd < 0 ? errno : 0;
        if (memoryFd < 0)
            LOGW("NCE fallback: safe instruction reads unavailable (errno={}); SIGILL recovery will not be attempted", memoryOpenError);
        logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (logFd < 0) {
            LOGW("NCE fallback: couldn't open '{}' for writing (errno={}); failures won't be recorded", logPath, errno);
            return;
        }
        WriteLine(fmt::format(R"({{"type":"session","schema_version":2,"session_id":"{}","game":"{}","unix_ns":{},"monotonic_ns":{},"pid":{},"jit_compiled":{},"opcode_reader":"proc_self_mem_pread","opcode_reader_errno":{},"max_tracked_keys":{},"max_session_log_bytes":{},"count_policy":"cumulative_powers_of_two_then_final","limitations":["signal_path_not_async_signal_safe","guest_data_accesses_not_fully_validated","no_memory_rollback","jit_trace_not_equivalence_proof"]}})" "\n",
                              sessionId, JsonEscape(gameName), TimestampNs(CLOCK_REALTIME), TimestampNs(CLOCK_MONOTONIC), getpid(),
#ifdef STRATO_JIT_FALLBACK
                              "true"
#else
                              "false"
#endif
                              , memoryOpenError, MaxTrackedFailures, MaxLogBytes));
    }

    void JitFallback::Flush() noexcept {
        try {
            std::scoped_lock lock{logMutex};
            FlushLocked();
        } catch (...) {
            // Called only after guest threads have stopped. A diagnostic allocation failure must not escape JNI teardown.
            if (logFd >= 0) {
                close(logFd);
                logFd = -1;
            }
            if (memoryFd >= 0) {
                close(memoryFd);
                memoryFd = -1;
            }
            constexpr char message[]{"NCE diagnostics: final counts unavailable after an exception\n"};
            [[maybe_unused]] auto result{write(STDERR_FILENO, message, sizeof(message) - 1)};
        }
    }

    bool JitFallback::HandleFault(int signal, siginfo *info, ucontext *ctx, ThreadContext &threadCtx) {
        const int savedErrno{errno};
        totalFaults.fetch_add(1, std::memory_order_relaxed);
        const FaultSnapshot fault{CaptureFault(signal, *info, *ctx)};
        Outcome outcome{Outcome::NotAttempted};
        std::string trace;
        std::string detail;
        if (signal != SIGILL)
            detail = "signal is not eligible for instruction emulation";
        else if (info->si_code <= 0)
            detail = "software-generated SIGILL is not executed by the fallback";
        else if (fault.instructionReadError)
            detail = fmt::format("original opcode could not be read safely (errno={}); JIT not attempted", fault.instructionReadError);
        else
            detail = "JIT not compiled in (build with -DSTRATO_JIT_FALLBACK=ON)";

#ifdef STRATO_JIT_FALLBACK
        if (signal == SIGILL && info->si_code > 0 && !fault.instructionReadError) {
            StepJob job{ctx, &threadCtx, &fault, Outcome::NotAttempted, {}};
            CallOnStack(&StepOnHostStack, &job, threadCtx.hostSp);
            outcome = job.outcome;
            detail = job.detail.empty() ? "single-stepped with Dynarmic" : job.detail;
            trace = std::move(job.trace);
        }
#endif

        Log(fault, *ctx, outcome, detail, trace);
        errno = savedErrno;
        return outcome == Outcome::Recovered;
    }
}
