// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
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
#include <kernel/scheduler.h>
#include <kernel/types/KProcess.h>
#include <kernel/types/KThread.h>

namespace skyline::nce {
    /**
     * @brief A diagnostic tool which logs everything needed to understand a frozen or very slow game from one run
     * @details Every period (10 s) it logs a summary line with the memory use and, for every thread that used CPU, its user/kernel CPU split and wait channel.
     * Guest threads (HOS-N) above HotThreadCpuPercent are interrupted with SIGUSR2 to read where they are (PC, LR, call stack, callee-saved registers) and the
     * instructions and memory those registers point to, which for a spin-wait shows the lock word they wait for.
     * A "full report" is written on the first hot period and every 6th one after it (or at the 6th period and every 12th after it when nothing is spinning):
     *  - every guest thread with its priority, core, affinity, scheduling and wait state (T<id> matches the HOS-<id> thread name)
     *  - the run queue of every scheduler core (the first thread of a queue is the one that owns the core)
     *  - the threads waiting on address arbiter / condition variable keys, with the key address
     *  - the call stack (host and guest frames) of every guest thread that isn't spinning, the GPU thread and the main thread, which for a thread blocked in a SVC
     *    shows which game function called it and what it is waiting on
     * Everything is read without stopping the emulation, so values can be momentarily inconsistent, but locks are only ever try-locked and memory is read through
     * /proc/self/mem so the watchdog cannot deadlock or fault the process. Header-only, the only changes elsewhere are two friend declarations.
     */
    class HangWatchdog {
      private:
        static constexpr int SampleSignal{SIGUSR2}; //!< The signal used to interrupt a thread and read its registers
        static constexpr u64 HotThreadCpuPercent{50}; //!< Guest threads using at least this much CPU over the period are sampled
        static constexpr size_t MaxHotThreads{6}; //!< Maximum number of spinning threads sampled per period
        static constexpr size_t SamplesPerThread{3};
        static constexpr size_t MaxFrames{20};
        static constexpr auto SampleTimeout{std::chrono::milliseconds{50}};
        static constexpr auto SampleSpacing{std::chrono::milliseconds{10}};

        struct Sample {
            u64 utime{}; //!< User CPU time in clock ticks at the previous sample
            u64 stime{}; //!< Kernel CPU time in clock ticks at the previous sample
        };

        using ThreadList = std::vector<std::pair<int, std::string>>; //!< (host tid, thread name)

        /**
         * @brief The registers captured by the sampling signal handler, only one request is ever in flight
         */
        struct Capture {
            std::atomic<bool> done;
            std::atomic<int> expectedTid; //!< Only this thread's answer is recorded so a late signal on another thread can't corrupt the capture
            bool guest;
            u64 pc, lr, sp, fp;
            u64 saved[10]; //!< X19-X28, callee-saved registers which usually hold the pointers a loop works on (e.g. a lock word)
            kernel::type::KThread *thread; //!< The KThread of the interrupted thread, only set for guest threads
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
            if (static_cast<int>(syscall(SYS_gettid)) != capture.expectedTid.load(std::memory_order_relaxed))
                return;

            auto &mctx{context->uc_mcontext};
            capture.guest = guest;
            capture.pc = mctx.pc;
            capture.lr = mctx.regs[30];
            capture.sp = mctx.sp;
            capture.fp = mctx.regs[29];
            for (size_t i{}; i < 10; i++)
                capture.saved[i] = mctx.regs[19 + i];
            capture.done.store(true, std::memory_order_release);
        }

        /**
         * @brief Called when the signal interrupts guest code, the guest TLS has already been swapped for the host one by the caller
         */
        static void GuestSampleHandler(int, siginfo *, ucontext *context, void **) {
            capture.thread = DeviceState::thread.get();
            Record(context, true);
        }

        /**
         * @brief Called when the signal interrupts host code (e.g. inside a SVC), the TLS may not be valid so stack protector is disabled and nothing but stores is done
         */
        static void __attribute__((no_stack_protector)) HostSampleHandler(int, siginfo *, ucontext *context) {
            capture.thread = nullptr;
            Record(context, false);
        }

        /**
         * @brief Interrupts a thread and captures the registers it was executing with
         * @return True if the thread answered in time, false otherwise (e.g. it is in a state where it can't take the signal right now)
         */
        static bool SampleThread(int tid) {
            capture.done.store(false, std::memory_order_relaxed);
            capture.expectedTid.store(tid, std::memory_order_relaxed);
            if (syscall(SYS_tgkill, getpid(), tid, SampleSignal) != 0)
                return false;

            auto deadline{std::chrono::steady_clock::now() + SampleTimeout};
            while (!capture.done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    capture.expectedTid.store(0, std::memory_order_relaxed);
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            capture.expectedTid.store(0, std::memory_order_relaxed);
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
         * @brief Reads memory of this process through /proc/self/mem so an unmapped address just fails instead of faulting
         */
        static bool ReadMemory(u64 address, void *buffer, size_t size) {
            int fd{open("/proc/self/mem", O_RDONLY | O_CLOEXEC)};
            if (fd < 0)
                return false;
            bool ok{pread(fd, buffer, size, static_cast<off_t>(address)) == static_cast<ssize_t>(size)};
            close(fd);
            return ok;
        }

        static bool ReadWord(u64 address, u32 &word) {
            return ReadMemory(address, &word, sizeof(word));
        }

        /**
         * @brief Formats the raw 32-bit instruction words in [address - before, address + after) with the one at 'address' in brackets
         */
        static std::string DumpWords(u64 address, size_t before, size_t after) {
            std::string out;
            for (u64 cursor{address - before}; cursor < address + after; cursor += sizeof(u32)) {
                u32 word{};
                if (!ReadWord(cursor, word))
                    out += " ????????";
                else
                    out += (cursor == address) ? fmt::format(" [{:08x}]", word) : fmt::format(" {:08x}", word);
            }
            return out;
        }

        /**
         * @brief Formats 'count' 64-bit values starting at 'address', four per line
         */
        static std::string DumpQwords(u64 address, size_t count) {
            std::string out;
            for (size_t i{}; i < count; i += 4) {
                out += fmt::format("\n      0x{:X}:", address + i * sizeof(u64));
                for (size_t j{i}; j < std::min(i + 4, count); j++) {
                    u64 value{};
                    out += ReadMemory(address + j * sizeof(u64), &value, sizeof(value)) ? fmt::format(" {:016X}", value) : " ????????????????";
                }
            }
            return out;
        }

        /**
         * @brief The call stack of the last captured thread: PC, LR and the frame pointer chain, which spans host frames (SVC handling) and the guest frames below them
         */
        static std::vector<void *> CollectFrames() {
            std::vector<void *> frames{reinterpret_cast<void *>(capture.pc)};
            if (capture.lr)
                frames.push_back(reinterpret_cast<void *>(capture.lr));

            u64 fp{capture.fp};
            for (size_t i{}; i < MaxFrames && fp >= 0x10000 && !(fp & 7); i++) {
                u64 record[2]; // {previous frame pointer, return address}
                if (!ReadMemory(fp, record, sizeof(record)) || !record[1])
                    break;
                if (reinterpret_cast<void *>(record[1]) != frames.back())
                    frames.push_back(reinterpret_cast<void *>(record[1]));
                if (record[0] <= fp)
                    break;
                fp = record[0];
            }
            return frames;
        }

        static std::string Symbolize(const DeviceState &state, const std::vector<void *> &frames) {
            try {
                return state.loader->GetStackTrace(frames);
            } catch (...) {
                return "\n* (symbol resolution failed)";
            }
        }

        /**
         * @brief A one line description of the scheduling state of a guest thread, fields are read without synchronization so they can be momentarily inconsistent
         */
        static std::string DescribeThread(kernel::type::KThread *thread, const std::string &host = {}) {
            if (!thread)
                return "(unknown)";
            return fmt::format("T{:<3} handle 0x{:X} prio {}/{} core {} ideal {} aff 0x{:X} | running {} ready {} killed {} paused {} preempted {} yield {}{} | waitMutex 0x{:X} waitThread {} waiters {} condSignalled {} wakeObject {} cancellable {} | host {}",
                               thread->id, thread->handle, static_cast<int>(thread->priority.load()), static_cast<int>(thread->basePriority.load()), thread->coreId, thread->idealCore,
                               thread->affinityMask.to_ullong(), thread->running, thread->ready, thread->killed, thread->isPaused, thread->isPreempted, thread->pendingYield, thread->forceYield ? "+force" : "",
                               reinterpret_cast<u64>(thread->waitMutex), thread->waitThread ? fmt::format("T{}", thread->waitThread->id) : "-", thread->waiters.size(), thread->waitSignalled,
                               thread->wakeObject ? "yes" : "no", thread->isCancellable, host.empty() ? "?" : host);
        }

        /**
         * @brief Describes what the saved registers point to, which for a lock-wait loop reveals the lock word, its value and (if the value is a thread handle) its owner
         */
        static std::string DescribeRegisters(const DeviceState &state, kernel::type::KThread *self, bool withMemory) {
            constexpr u32 HandleWaitersBit{1U << 30}; // Set in a guest mutex value when other threads are waiting on it, the rest is the owner's handle
            std::string out{fmt::format("\n  this thread: {}", DescribeThread(self))};
            for (size_t i{}; i < 10; i++) {
                u64 value{capture.saved[i]};
                u32 word{};
                if (value < 0x10000 || value >= (1ULL << 39) || (value & 3) || !ReadWord(value, word))
                    continue;

                out += fmt::format("\n  X{} = 0x{:X} -> [0x{:X}] = 0x{:X}", 19 + i, value, value, word);
                if (withMemory && value >= 0x10020)
                    out += DumpQwords(value - 0x20, 12);
                if (u32 owner{word & ~HandleWaitersBit}; owner) {
                    try {
                        auto thread{state.process->GetHandle<kernel::type::KThread>(owner)};
                        out += fmt::format("\n    if this is a lock, owner handle 0x{:X} = {}", owner, DescribeThread(thread.get()));
                    } catch (...) {
                    }
                }
            }
            return out;
        }

        /**
         * @brief Every guest thread with its scheduling and wait state, the host state of its backing thread is appended from 'hostInfo'
         */
        static std::string DumpThreadTable(const DeviceState &state, const std::unordered_map<std::string, std::string> &hostInfo) {
            if (!state.process)
                return "\n  (no process)";
            std::unique_lock lock{state.process->threadMutex, std::try_to_lock};
            if (!lock.owns_lock())
                return "\n  (thread list is locked right now)";

            std::string out;
            for (const auto &thread : state.process->threads) {
                if (!thread)
                    continue;
                auto it{hostInfo.find(fmt::format("HOS-{}", thread->id))};
                out += "\n  " + DescribeThread(thread.get(), it != hostInfo.end() ? it->second : std::string{});
            }
            return out;
        }

        /**
         * @brief The run queue of every scheduler core, the first thread of a queue is the one that owns the core
         */
        static std::string DumpSchedulerQueues(const DeviceState &state) {
            if (!state.scheduler)
                return "\n  (no scheduler)";

            std::string out;
            for (size_t i{}; i < state.scheduler->cores.size(); i++) {
                auto &core{state.scheduler->cores[i]};
                std::unique_lock lock{core.mutex, std::try_to_lock};
                if (!lock.owns_lock()) {
                    out += fmt::format("\n  core {}: queue is locked right now", i);
                    continue;
                }
                out += fmt::format("\n  core {} (preemptive from priority {}):", i, static_cast<int>(core.preemptionPriority));
                for (const auto &thread : core.queue)
                    out += fmt::format(" T{}(p{})", thread->id, static_cast<int>(thread->priority.load()));
            }

            std::unique_lock lock{state.scheduler->parkedMutex, std::try_to_lock};
            if (lock.owns_lock()) {
                out += "\n  parked:";
                for (const auto &thread : state.scheduler->parkedQueue)
                    out += fmt::format(" T{}", thread->id);
            }
            return out;
        }

        /**
         * @brief The threads waiting on process-wide keys (address arbiter and condition variables), grouped by the guest address they wait on
         */
        static std::string DumpSyncWaiters(const DeviceState &state) {
            if (!state.process)
                return "\n  (no process)";
            std::unique_lock lock{state.process->syncWaiterMutex, std::try_to_lock};
            if (!lock.owns_lock())
                return "\n  (waiter list is locked right now)";

            std::string out, line;
            void *lastKey{};
            size_t keys{};
            for (const auto &[key, thread] : state.process->syncWaiters) {
                if (key != lastKey || line.empty()) {
                    if (!line.empty())
                        out += line;
                    if (++keys > 80) {
                        line.clear();
                        out += "\n  (more keys omitted)";
                        break;
                    }
                    line = fmt::format("\n  key 0x{:X}:", reinterpret_cast<u64>(key));
                    lastKey = key;
                }
                line += fmt::format(" T{}", thread->id);
            }
            out += line;
            return out.empty() ? "\n  (none)" : out;
        }

        /**
         * @brief Samples the spinning threads, with 'full' also logs the instructions and memory their registers refer to
         */
        static void SampleHotThreads(const DeviceState &state, u64 index, const ThreadList &hotThreads, bool full) {
            for (const auto &[tid, name] : hotThreads) {
                std::string report;
                bool detailed{};
                for (size_t i{}; i < SamplesPerThread; i++) {
                    if (SampleThread(tid)) {
                        if (full && !detailed && capture.guest) {
                            detailed = true;
                            report += fmt::format("\n  code at PC 0x{:X}:{}\n  code before LR 0x{:X}:{}{}", capture.pc, DumpWords(capture.pc, 16, 16), capture.lr, DumpWords(capture.lr, 16, 4), DescribeRegisters(state, capture.thread, true));
                        }
                        report += fmt::format("\n  sample {} [{}] SP 0x{:X} FP 0x{:X}{}", i + 1, capture.guest ? "guest" : "host", capture.sp, capture.fp, Symbolize(state, CollectFrames()));
                    } else {
                        report += fmt::format("\n  sample {}: no answer within {} ms", i + 1, SampleTimeout.count());
                    }
                    std::this_thread::sleep_for(SampleSpacing);
                }
                LOGINF("Watchdog #{}: where {} (tid {}) is running:{}", index, name, tid, report);
            }
        }

        /**
         * @brief Logs the call stack (host and guest frames) of every given thread
         */
        static void DumpThreadStacks(const DeviceState &state, u64 index, const ThreadList &threads) {
            for (const auto &[tid, name] : threads) {
                if (!SampleThread(tid)) {
                    LOGINF("Watchdog #{}: stack of {} (tid {}): no answer", index, name, tid);
                    continue;
                }
                LOGINF("Watchdog #{}: stack of {} (tid {}) [{}]:{}", index, name, tid, capture.guest ? "guest" : "host", Symbolize(state, CollectFrames()));
                std::this_thread::sleep_for(SampleSpacing);
            }
        }

        static void Run(const DeviceState *state, std::chrono::seconds period) {
            pthread_setname_np(pthread_self(), "Sky-Watchdog");
            AsyncLogger::UpdateTag();

            const long ticksPerSecond{sysconf(_SC_CLK_TCK)};
            const long pageSize{sysconf(_SC_PAGESIZE)};
            std::unordered_map<int, Sample> previous;
            u64 index{}, hotPeriods{};

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
                ThreadList hotThreads, stackThreads;
                std::unordered_map<std::string, std::string> hostInfo;

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
                        std::string threadState;
                        fields >> threadState; // Field 3
                        std::string skip;
                        for (int i{4}; i <= 13; i++)
                            fields >> skip;
                        u64 utime{}, stime{};
                        fields >> utime >> stime; // Fields 14 and 15

                        switch (threadState.empty() ? '?' : threadState[0]) {
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

                        const u64 window{static_cast<u64>(ticksPerSecond) * static_cast<u64>(period.count())};
                        u64 percent{delta * 100 / window};
                        std::string name{ReadFile(base + "comm")};
                        bool guestThread{name.starts_with("HOS-")};

                        hostInfo[name] = fmt::format("{} cpu {}%", threadState, percent);
                        if (guestThread || name == "GPFIFO" || name == "EmuMain")
                            stackThreads.emplace_back(tid, name);

                        // Only threads that did work or are in uninterruptible sleep are listed, idle threads would drown out the interesting ones
                        if (delta > 0 || threadState[0] == 'D') {
                            std::string wchan{ReadFile(base + "wchan")};
                            details += fmt::format("\n    tid {:>6} {:<16} state {} cpu {:>3}% (user {:>3}% sys {:>3}%) wchan {}", tid, name, threadState, percent, userDelta * 100 / window, sysDelta * 100 / window, wchan.empty() ? "-" : wchan);

                            if (samplerEnabled && guestThread && percent >= HotThreadCpuPercent && hotThreads.size() < MaxHotThreads)
                                hotThreads.emplace_back(tid, name);
                        }
                    }
                    closedir(dir);
                }

                index++;
                LOGINF("Watchdog #{}: RSS {} MiB, threads: {} running, {} sleeping, {} disk-wait, {} other{}", index, rssPages * pageSize / (1024 * 1024), running, sleeping, disk, other, details);

                bool full{!hotThreads.empty() ? (hotPeriods++ % 6) == 0 : (index == 6 || (index > 6 && index % 12 == 0))};
                if (full) {
                    LOGINF("Watchdog #{}: FULL REPORT ({} spinning guest threads)\n guest threads:{}\n scheduler queues:{}\n address arbiter / condition variable waiters:{}", index, hotThreads.size(), DumpThreadTable(*state, hostInfo), DumpSchedulerQueues(*state), DumpSyncWaiters(*state));
                }

                if (!hotThreads.empty())
                    SampleHotThreads(*state, index, hotThreads, full);

                if (full && samplerEnabled) {
                    ThreadList others;
                    for (const auto &candidate : stackThreads)
                        if (std::none_of(hotThreads.begin(), hotThreads.end(), [&](const auto &hot) { return hot.first == candidate.first; }))
                            others.push_back(candidate);
                    DumpThreadStacks(*state, index, others);
                }
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
