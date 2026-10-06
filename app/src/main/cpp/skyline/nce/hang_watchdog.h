// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <common.h>

namespace skyline::nce {
    /**
     * @brief Periodically logs what every host thread of the process is doing so a frozen game can be diagnosed from the normal log
     * @details Every period it reads /proc/self/task/<tid>/{stat,wchan,comm} and writes one summary line (RSS, thread counts) followed by one
     * line per thread that used CPU since the last sample. A thread that is stuck waiting shows state S/D and no CPU, one that spins in guest
     * code shows state R and a high CPU percentage. Header-only on purpose, it needs no build system change.
     */
    class HangWatchdog {
      private:
        struct Sample {
            u64 ticks{}; //!< utime + stime in clock ticks at the previous sample
        };

        static inline std::thread thread;
        static inline std::mutex mutex;
        static inline std::condition_variable cv;
        static inline bool stop{true};

        static std::string ReadFile(const std::string &path) {
            std::ifstream file{path};
            std::stringstream out;
            out << file.rdbuf();
            std::string text{out.str()};
            while (!text.empty() && (text.back() == '\n' || text.back() == '\0'))
                text.pop_back();
            return text;
        }

        static void Run(std::chrono::seconds period) {
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

                        u64 ticks{utime + stime};
                        auto it{previous.find(tid)};
                        u64 delta{it != previous.end() ? ticks - it->second.ticks : 0};
                        previous[tid].ticks = ticks;

                        // Only threads that did work or are in uninterruptible sleep are listed, idle threads would drown out the interesting ones
                        if (delta > 0 || state[0] == 'D') {
                            std::string name{ReadFile(base + "comm")};
                            std::string wchan{ReadFile(base + "wchan")};
                            details += fmt::format("\n    tid {:>6} {:<16} state {} cpu {:>3}% wchan {}", tid, name, state, delta * 100 / (ticksPerSecond * period.count()), wchan.empty() ? "-" : wchan);
                        }
                    }
                    closedir(dir);
                }

                LOGINF("Watchdog #{}: RSS {} MiB, threads: {} running, {} sleeping, {} disk-wait, {} other{}", ++index, rssPages * pageSize / (1024 * 1024), running, sleeping, disk, other, details);
            }
        }

      public:
        static void Start(std::chrono::seconds period = std::chrono::seconds{10}) {
            std::scoped_lock lock{mutex};
            if (thread.joinable())
                return;
            stop = false;
            thread = std::thread{&HangWatchdog::Run, period};
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
