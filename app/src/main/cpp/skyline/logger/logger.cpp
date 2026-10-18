// SPDX-License-Identifier: GPL-3.0-or-later
#include "logger.h"
#include <common/diagnostic_recorder.h>
#include <android/log.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <thread>

namespace skyline {
    namespace {
        using Clock = std::chrono::steady_clock;
        using Level = AsyncLogger::LogLevel;
        constexpr size_t MaxMessageBytes{8192};
        constexpr size_t ReservedErrors{64};
        constexpr size_t MaxRateKeys{1024};
        constexpr unsigned Burst{8};
        thread_local std::array<char, 16> threadName{"unknown"};
        thread_local long threadId{};
        std::mutex lifecycleMutex;
        std::atomic<Level> minimumLevel{Level::Disabled};
        std::atomic<uint64_t> unavailableDrops{};
        struct Message {
            Level level;
            std::string text, component, site, function;
            long tid;
            std::array<char, 16> name;
            Clock::time_point time;
        };
        std::string Component(const char *file) {
            if (!file) return "external";
            std::string_view path{file};
            auto start{path.find("skyline/")};
            if (start == path.npos) return "native";
            path.remove_prefix(start + 8);
            auto slash{path.find('/')};
            if (slash != path.npos) return std::string{path.substr(0, slash)};
            return std::string{path.substr(0, path.find('.'))};
        }
        void Emergency(const char *message) noexcept {
            __android_log_write(ANDROID_LOG_ERROR, "emu-cpp-logger", message);
        }
    }

    class AsyncLogger::Impl {
        std::mutex queueMutex;
        std::condition_variable condition;
        std::array<std::optional<Message>, LogQueueSize> queue;
        size_t head{}, tail{}, count{};
        bool stopping{};
        std::atomic<uint64_t> dropped{}, truncated{};
        uint64_t suppressed{}, outputErrors{};
        bool fileFailed{};
        std::ofstream file;
        const Clock::time_point start{Clock::now()};
        std::thread writer;
        struct Rate { Clock::time_point start; unsigned emitted{}; uint64_t skipped{}; };
        std::map<std::string, Rate> rates; // Writer-owned, bounded; no producer formatting under this lock.

        void Output(const Message &m, std::string_view text) {
            constexpr std::array<const char *, 5> levels{"VERBOSE", "DEBUG", "INFO", "WARNING", "ERROR"};
            constexpr std::array<int, 5> androidLevels{ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
            auto index{static_cast<size_t>(m.level)};
            auto body{fmt::format("[tid={}][{}][{}] {}{}{}", m.tid, m.component, m.site,
                                  m.function, m.function.empty() ? "" : ": ", text)};
            __android_log_write(androidLevels[index], "emu-cpp", body.c_str());
            if (fileFailed) return;
            // Keep the four existing columns and numeric level ordering for log readers.
            file << fmt::format("{:7} | {:>10} | {:^15} | {}\n", levels[index],
                std::chrono::duration_cast<std::chrono::microseconds>(m.time - start).count(), m.name.data(), body);
            if (!file) FileError();
        }
        void FileError() noexcept {
            if (!fileFailed) { fileFailed = true; ++outputErrors; Emergency("Log file write/open failed; continuing to logcat"); }
        }
        void Flush() {
            if (!fileFailed) { file.flush(); if (!file) FileError(); }
        }
        void RateSummary(const std::string &key, Rate &rate) {
            if (!rate.skipped) return;
            Message summary{Level::Warning, {}, "logger", key, {}, syscall(SYS_gettid), {}, Clock::now()};
            Output(summary, fmt::format("rate_limited: {} additional messages suppressed at this site", rate.skipped));
            rate.skipped = 0;
        }
        void Process(Message &message) {
            // A stable source location bounds even messages with changing addresses.
            // Direct legacy callers fall back to function/component + a bounded text prefix.
            auto key{fmt::format("{}:{}:{}", message.component, message.site, static_cast<int>(message.level))};
            if (message.site == "external:0") key += ':' + message.function + ':' + message.text.substr(0, 80);
            auto it{rates.find(key)};
            if (it == rates.end()) {
                if (rates.size() >= MaxRateKeys) {
                    RateSummary(rates.begin()->first, rates.begin()->second);
                    rates.erase(rates.begin());
                }
                it = rates.emplace(key, Rate{Clock::now()}).first;
            }
            auto &rate{it->second};
            auto now{Clock::now()};
            if (now - rate.start >= std::chrono::seconds(1)) {
                RateSummary(key, rate);
                rate = Rate{now};
            }
            if (rate.emitted >= Burst) { ++rate.skipped; ++suppressed; return; }
            ++rate.emitted;
            Output(message, message.text);
        }
        void Run() noexcept {
            pthread_setname_np(pthread_self(), "Sky-Logger");
            auto nextFlush{Clock::now() + std::chrono::milliseconds(200)};
            try {
                for (;;) {
                    std::optional<Message> message;
                    {
                        std::unique_lock lock{queueMutex};
                        condition.wait_until(lock, nextFlush, [this] { return stopping || count; });
                        if (count) {
                            message = std::move(queue[head]);
                            queue[head].reset();
                            head = (head + 1) % LogQueueSize;
                            --count;
                        } else if (stopping) break;
                    }
                    if (message) Process(*message);
                    if (Clock::now() >= nextFlush) {
                        for (auto &[key, rate] : rates)
                            if (Clock::now() - rate.start >= std::chrono::seconds(1)) RateSummary(key, rate);
                        Flush();
                        nextFlush = Clock::now() + std::chrono::milliseconds(200);
                    }
                }
                for (auto &[key, rate] : rates) RateSummary(key, rate);
                Message summary{Level::Info, {}, "logger", "shutdown", {}, syscall(SYS_gettid), {}, Clock::now()};
                Output(summary, fmt::format("logger_stats dropped={} unavailable={} suppressed={} truncated={} output_errors={}",
                    dropped.load(), unavailableDrops.load(), suppressed, truncated.load(), outputErrors));
                Flush();
            } catch (...) {
                Emergency("Logger worker failed; further queued messages will be dropped");
                std::lock_guard lock{queueMutex}; stopping = true;
            }
        }
      public:
        explicit Impl(const std::filesystem::path &path) {
            std::error_code error;
            if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
            file.open(path, std::ios::trunc);
            if (error || !file) FileError();
            writer = std::thread{&Impl::Run, this};
        }
        ~Impl() { Stop(); }
        void Stop() {
            { std::lock_guard lock{queueMutex}; stopping = true; }
            condition.notify_all();
            if (writer.joinable()) writer.join();
        }
        void Push(Message message) noexcept {
            try {
                if (message.text.size() > MaxMessageBytes) {
                    message.text.resize(MaxMessageBytes - 14); message.text += " [truncated]"; ++truncated;
                }
                std::unique_lock lock{queueMutex, std::try_to_lock};
                if (!lock || stopping || count == LogQueueSize ||
                    (message.level < Level::Warning && count >= LogQueueSize - ReservedErrors)) {
                    ++dropped; return;
                }
                queue[tail].emplace(std::move(message));
                tail = (tail + 1) % LogQueueSize; ++count;
                lock.unlock(); condition.notify_one();
            } catch (...) { ++dropped; }
        }
    };
    namespace { std::shared_ptr<AsyncLogger::Impl> instance; }

    void AsyncLogger::Initialize(LogLevel level, const std::filesystem::path &path) {
        std::lock_guard lock{lifecycleMutex};
        minimumLevel = Level::Disabled;
        if (instance) instance->Stop();
        instance.reset();
        unavailableDrops = 0;
        if (level >= Level::Verbose && level < Level::Disabled) {
            instance = std::make_shared<Impl>(path);
            minimumLevel = level;
        }
        UpdateTag();
    }
    void AsyncLogger::Finalize(bool /*wait*/) {
        // Both legacy modes drain and join. No detached worker may outlive its storage.
        std::lock_guard lock{lifecycleMutex};
        minimumLevel = Level::Disabled;
        if (instance) instance->Stop();
        instance.reset();
    }
    void AsyncLogger::UpdateTag() {
        threadId = syscall(SYS_gettid);
        if (pthread_getname_np(pthread_self(), threadName.data(), threadName.size()) != 0)
            threadName = {'u','n','k','n','o','w','n',0};
    }
    bool AsyncLogger::CheckLogLevel(LogLevel level) {
        return level >= minimumLevel.load(std::memory_order_relaxed) && level < Level::Disabled;
    }
    void AsyncLogger::LogAsync(LogLevel level, std::string &&str, const char *function, const char *source, unsigned line) {
        if (!CheckLogLevel(level)) return;
        try {
            std::shared_ptr<Impl> target;
            {
                std::unique_lock lock{lifecycleMutex, std::try_to_lock};
                if (!lock || !instance) { ++unavailableDrops; return; }
                target = instance;
            }
            if (!threadId) UpdateTag();
            if (!source) diagnostics::ObserveLog(function ? function : "log", str);
            target->Push(Message{level, std::move(str), Component(source),
                fmt::format("{}:{}", source ? source : "external", line), function ? function : "",
                threadId, threadName, Clock::now()});
        } catch (...) { ++unavailableDrops; }
    }
    void AsyncLogger::ReportUnsupported(const char *source, unsigned line, std::string_view detail) noexcept {
        diagnostics::Unsupported(source ? source : "external",line,detail);
    }
    void AsyncLogger::LogSync(LogLevel level, std::string &&str, const char *function) {
        // Legacy emergency API: logcat only; never race the writer's file stream.
        if (!CheckLogLevel(level)) return;
        constexpr std::array<int, 5> levels{ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        try {
            if (!threadId) UpdateTag();
            auto body{fmt::format("[tid={}][sync] {}{}{}", threadId, function ? function : "", function ? ": " : "", str.substr(0, MaxMessageBytes))};
            __android_log_write(levels[static_cast<size_t>(level)], "emu-cpp", body.c_str());
            LogAsync(level, std::move(str), function);
        } catch (...) { Emergency("Synchronous log formatting failed"); }
    }
}
