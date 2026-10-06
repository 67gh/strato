// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <condition_variable>
#include <unordered_map>
#include <csignal>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <common.h>
#include <common/signal.h>
#include <loader/loader.h>

namespace skyline::nce {
    /**
     * @brief Periodically logs what every host thread of the process is doing so a frozen game can be diagnosed from the normal log
     * @details Every period it reads /proc/self/task/<tid>/{stat,wchan,comm} and writes one summary line (RSS, thread counts) followed by one
     * line per thread that used CPU since the last sample. A thread that is stuck waiting shows state S/D and no CPU, one that spins in guest
     * code shows state R and a high CPU percentage.
     * For guest threads (HOS-N) above HotThreadCpuPercent it additionally interrupts the thread with SIGUSR2 a few times and logs where it was
     * (PC, LR, SP, FP, X0-X3) with the module/symbol resolved by the loader, which shows what a spinning guest thread is waiting for.
     * Header-only on purpose, it needs no build system change.
     */
    class HangWatchdog {
      private:
        static constexpr int SampleSignal{SIGUSR2}; //!< The signal used to interrupt a thread and read its registers
        static constexpr u64 HotThreadCpuPercent{50}; //!< Threads using at least this much CPU over the period are sampled
        static constexpr size_t MaxSampledThreads{4}; //!< Maximum number of threads sampled per period
        static constexpr size_t SamplesPerThread{3};
        static constexpr auto SampleTimeout{std::chrono::milliseconds{50}};
        static constexpr auto SampleSpacing{std::chrono::milliseconds{10}};

        struct Sample {
            u64 utime{}; //!< User CPU time in clock ticks at the previous sample
            u64 stime{}; //!< Kernel CPU time in clock ticks at the previous sample
        };

        /**
         * @brief The registers captured by the sampling signal handler, only one request is ever in flight
         */
        struct Capture {
            std::atomic<bool> done;
            bool guest;
            u64 pc, lr, sp, fp, x0, x1, x2, x3;
        };

        static inline Capture capture{};
        static inline bool samplerEnabled{};

        static inline std::thread thread;
        static inline std::mutex mutex;
        static inline std::condition_variable cv;
        static inline bool stop{true};

        /**
         * @brief Records the registers of the interrupted context, this runs on the interrupted thread so it only does plain stores
         */
        static void Record(ucontext *context, bool guest) {
            auto &mctx{context->uc_mcontext};
            capture.guest = guest;
            capture.pc = mctx.pc;
            capture.lr = mctx.regs[30];
            capture.sp = mctx.sp;
            capture.fp = mctx.regs[29];
            capture.x0 = mctx.regs[0];
            capture.x1 = mctx.regs[1];
            capture.x2 = mctx.regs[2];
            capture.x3 = mctx.regs[3];
            capture.done.store(true, std::memory_order_release);
        }

        /**
         * @brief Called when the signal interrupts guest code, the guest TLS has already been swapped for the host one by the caller
         */
        static void GuestSampleHandler(int, siginfo *, ucontext *context, void **) {
            Record(context, true);
        }

        /**
         * @brief Called when the signal interrupts host code (e.g. inside a SVC), the TLS may not be valid so stack protector is disabled and nothing but stores is done
         */
        static void __attribute__((no_stack_protector)) HostSampleHandler(int, siginfo *, ucontext *context) {
            Record(context, false);
        }

        /**
         * @brief Interrupts a thread and returns the registers it was executing with
         * @return True if the thread answered in time, false otherwise (e.g. it went to sleep and the signal is still pending)
         */
        static bool SampleThread(int tid) {
            capture.done.store(false, std::memory_order_relaxed);
            if (syscall(SYS_tgkill, getpid(), tid, SampleSignal) != 0)
                return false;

            auto deadline{std::chrono::steady_clock::now() + SampleTimeout};
            while (!capture.done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return true;
        }

        static std::string ReadFile(const std::string &path) {
            std::ifstream file{path};
            std::stringstream out;
            out << file.rdbuf();
            std::string text{out.str()};
            while (!text.empty() && (text.back() == '\n' || text.back() == '\0'))
                text.pop_back();
            return text;
        }

        /**
         * @brief Formats the raw 32-bit instruction words in [address - before, address + after) with the one at 'address' in brackets
         * @note Reads through /proc/self/mem so an unmapped address just fails instead of faulting
         */
        static std::string DumpWords(u64 address, size_t before, size_t after) {
            int fd{open("/proc/self/mem", O_RDONLY | O_CLOEXEC)};
            if (fd < 0)
                return "(cannot open /proc/self/mem)";

            std::string out;
            for (u64 cursor{address - before}; cursor < address + after; cursor += sizeof(u32)) {
                u32 word{};
                if (pread(fd, &word, sizeof(word), static_cast<off_t>(cursor)) != static_cast<ssize_t>(sizeof(word)))
                    out += " ????????";
                else
                    out += (cursor == address) ? fmt::format(" [{:08x}]", word) : fmt::format(" {:08x}", word);
            }
            close(fd);
            return out;
        }

        static void SampleHotThreads(const DeviceState &state, u64 index, const std::vector<std::pair<int, std::string>> &hotThreads) {
            for (const auto &[tid, name] : hotThreads) {
                std::string report;
                bool codeDumped{};
                for (size_t i{}; i < SamplesPerThread; i++) {
                    if (SampleThread(tid)) {
                        if (!codeDumped && capture.guest) {
                            codeDumped = true;
                            report += fmt::format("\n  code at PC 0x{:X}:{}\n  code before LR 0x{:X}:{}", capture.pc, DumpWords(capture.pc, 16, 16), capture.lr, DumpWords(capture.lr, 16, 4));
                        }
                        std::string frames;
                        try {
                            frames = state.loader->GetStackTrace(std::vector<void *>{reinterpret_cast<void *>(capture.pc), reinterpret_cast<void *>(capture.lr)});
                        } catch (...) {
                            frames = "\n* (symbol resolution failed)";
                        }
                        report += fmt::format("\n  sample {} [{}] SP 0x{:X} FP 0x{:X} X0 0x{:X} X1 0x{:X} X2 0x{:X} X3 0x{:X}{}", i + 1, capture.guest ? "guest" : "host", capture.sp, capture.fp, capture.x0, capture.x1, capture.x2, capture.x3, frames);
                    } else {
                        report += fmt::format("\n  sample {}: no answer within {} ms (thread is sleeping or in a non-interruptible state)", i + 1, SampleTimeout.count());
                    }
                    std::this_thread::sleep_for(SampleSpacing);
                }
                LOGINF("Watchdog #{}: where {} (tid {}) is running:{}", index, name, tid, report);
            }
        }

        static void Run(const DeviceState *state, std::chrono::seconds period) {
            pthread_setname_np(pthread_self(), "Sky-Watchdog");
            AsyncLogger::UpdateTag();

            const long ticksPerSecond{sysconf(_SC_CLK_TCK)};
            const long pageSize{sysconf(_SC_PAGESIZE)};
            std::unordered_map<int, Sample> previous;
            u64 index{};

            std::unique_lock lock{mutex};
            while (!stop) {
                if (cv.wait_for(lock, period, [] { return stop; }))
                    break;

                u64 rssPages{};
                {
                    std::istringstream statm{ReadFile("/proc/self/statm")};
                    u64 size{};
                    statm >> size >> rssPages;
                }

                u32 running{}, sleeping{}, disk{}, other{};
                std::string details;
                std::vector<std::pair<int, std::string>> hotThreads;

                if (auto *dir{opendir("/proc/self/task")}) {
                    while (auto *entry{readdir(dir)}) {
                        int tid{atoi(entry->d_name)};
                        if (tid <= 0)
                            continue;

                        std::string base{fmt::format("/proc/self/task/{}/", tid)};
                        std::string stat{ReadFile(base + "stat")};
                        auto close{stat.rfind(')')}; // The comm field may itself contain spaces and parentheses, so everything after the last ')' is parsed
                        if (close == std::string::npos)
                            continue;

                        std::istringstream fields{stat.substr(close + 2)};
                        std::string state;
                        fields >> state; // Field 3
                        std::string skip;
                        for (int i{4}; i <= 13; i++)
                            fields >> skip;
                        u64 utime{}, stime{};
                        fields >> utime >> stime; // Fields 14 and 15

                        switch (state.empty() ? '?' : state[0]) {
                            case 'R': running++; break;
                            case 'S': sleeping++; break;
                            case 'D': disk++; break;
                            default: other++; break;
                        }

                        auto it{previous.find(tid)};
                        u64 userDelta{it != previous.end() ? utime - it->second.utime : 0};
                        u64 sysDelta{it != previous.end() ? stime - it->second.stime : 0};
                        u64 delta{userDelta + sysDelta};
                        previous[tid] = Sample{utime, stime};

                        // Only threads that did work or are in uninterruptible sleep are listed, idle threads would drown out the interesting ones
                        if (delta > 0 || state[0] == 'D') {
                            std::string name{ReadFile(base + "comm")};
                            std::string wchan{ReadFile(base + "wchan")};
                            const u64 window{static_cast<u64>(ticksPerSecond) * static_cast<u64>(period.count())};
                            u64 percent{delta * 100 / window};
                            details += fmt::format("\n    tid {:>6} {:<16} state {} cpu {:>3}% (user {:>3}% sys {:>3}%) wchan {}", tid, name, state, percent, userDelta * 100 / window, sysDelta * 100 / window, wchan.empty() ? "-" : wchan);

                            if (samplerEnabled && percent >= HotThreadCpuPercent && name.starts_with("HOS-") && hotThreads.size() < MaxSampledThreads)
                                hotThreads.emplace_back(tid, name);
                        }
                    }
                    closedir(dir);
                }

                index++;
                LOGINF("Watchdog #{}: RSS {} MiB, threads: {} running, {} sleeping, {} disk-wait, {} other{}", index, rssPages * pageSize / (1024 * 1024), running, sleeping, disk, other, details);

                if (!hotThreads.empty())
                    SampleHotThreads(*state, index, hotThreads);
            }
        }

      public:
        static void Start(const DeviceState &state, std::chrono::seconds period = std::chrono::seconds{10}) {
            std::scoped_lock lock{mutex};
            if (thread.joinable())
                return;

            try {
                signal::SetGuestSignalHandler({SampleSignal}, GuestSampleHandler);
                signal::SetHostSignalHandler({SampleSignal}, HostSampleHandler);
                samplerEnabled = true;
            } catch (const std::exception &e) {
                LOGW("Watchdog: PC sampling disabled, couldn't install the signal handler: {}", e.what());
            }

            stop = false;
            thread = std::thread{&HangWatchdog::Run, &state, period};
        }

        /**
         * @brief Stops and joins the watchdog, must be called before the logger is finalized
         */
        static void Stop() {
            {
                std::scoped_lock lock{mutex};
                stop = true;
            }
            cv.notify_all();
            if (thread.joinable())
                thread.join();
        }
    };
}
