// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <common/format.h>

namespace skyline {
    /**
     * @brief The public interface of the logger. The logger writes logs into a log file and logcat using Android Log APIs.
     * The Initialize function must be called before using the logger and the Finalize function should be called before the program exits
     * to ensure proper flushing of all logs.
     * @details This is an Asnychronous logger, it pushes all logs into a queue and a background thread writes them out.
     */
    class AsyncLogger {
      public:
        class Impl;

        enum class LogLevel {
            Verbose,
            Debug,
            Info,
            Warning,
            Error,
            Disabled, // A special log level that disables all logging, must be the last log level
        };

        static constexpr bool IsUnsupportedFormat(std::string_view text) {
            return text.find("STUB")!=text.npos || text.find("stubbed")!=text.npos ||
                   text.find("unimplemented")!=text.npos || text.find("Unimplemented")!=text.npos ||
                   text.find("unsupported")!=text.npos || text.find("Unsupported")!=text.npos ||
                   text.find("not implemented")!=text.npos || text.find("Not implemented")!=text.npos;
        }
        static void ReportUnsupported(const char *source, unsigned line, std::string_view detail) noexcept;
        constexpr static size_t LogQueueSize{1024}; //!< The size of the log message queue

        /**
         * @brief Initializes the logger with the given log level and file path
         * @details This starts the writer thread
         */
        static void Initialize(LogLevel level, const std::filesystem::path &path);

        /**
         * @brief Finalizes the logger and flushes all pending logs
         * @details This stops the writer thread
         * @note After calling this, Initialize must be called before using the logger again
         * @param wait Retained for source compatibility. Both modes now drain and join safely.
         */
        static void Finalize(bool wait = false);

        /**
         * @brief Updates the log tag and thread name for the calling thread
         * @note This should be called whenever a new thread is created
         */
        static void UpdateTag();

        /**
         * @return <code>true</code> if the message with the given log level should be written, <code>false</code> otherwise
         */
        static bool CheckLogLevel(LogLevel level);

        /**
         * @brief Writes a log message to the log file and logcat asynchronously
         * @param function The name of the function that pushed this message, nullptr if no function should be prepended
         */
        static void LogAsync(LogLevel level, std::string &&str, const char *function = nullptr, const char *source = nullptr, unsigned line = 0);

        /**
         * @brief Writes immediately to logcat and queues the file copy; not signal-handler safe
         * @param function The name of the function that pushed this message, nullptr if no function should be prepended
         */
        static void LogSync(LogLevel level, std::string &&str, const char *function = nullptr);

      private:
        AsyncLogger() = default;
    };
}

// Register unsupported sites even when their text log level is disabled.
// Format strings identify the site without formatting changing addresses/arguments.
#define LOG_WRITE(level, formatString, ...) \
    do { \
        if (skyline::AsyncLogger::IsUnsupportedFormat(formatString)) \
            skyline::AsyncLogger::ReportUnsupported(__FILE__, __LINE__, formatString); \
        if (!skyline::AsyncLogger::CheckLogLevel(level)) break; \
        std::string _str = fmt::format(formatString __VA_OPT__(,) __VA_ARGS__); \
        skyline::AsyncLogger::LogAsync(level, std::move(_str), __builtin_FUNCTION(), __FILE__, __LINE__); \
    } while (0)

#define LOGNF_WRITE(level, formatString, ...) \
    do { \
        if (skyline::AsyncLogger::IsUnsupportedFormat(formatString)) \
            skyline::AsyncLogger::ReportUnsupported(__FILE__, __LINE__, formatString); \
        if (!skyline::AsyncLogger::CheckLogLevel(level)) break; \
        std::string _str = fmt::format(formatString __VA_OPT__(,) __VA_ARGS__); \
        skyline::AsyncLogger::LogAsync(level, std::move(_str), nullptr, __FILE__, __LINE__); \
    } while (0)

/**
 * @brief Logs an Error message, formatted using fmt::format
 */
#define LOGE(...) LOG_WRITE(skyline::AsyncLogger::LogLevel::Error, __VA_ARGS__)
/**
 * @brief Logs an Error message without the calling function name, formatted using fmt::format
 */
#define LOGENF(...) LOGNF_WRITE(skyline::AsyncLogger::LogLevel::Error, __VA_ARGS__)

/**
 * @brief Logs a Warning message, formatted using fmt::format
 */
#define LOGW(...) LOG_WRITE(skyline::AsyncLogger::LogLevel::Warning, __VA_ARGS__)
/**
 * @brief Logs a Warning message without the calling function name, formatted using fmt::format
 */
#define LOGWNF(...) LOGNF_WRITE(skyline::AsyncLogger::LogLevel::Warning, __VA_ARGS__)

/**
 * @brief Logs an Info message, formatted using fmt::format
 */
#define LOGI(...) LOG_WRITE(skyline::AsyncLogger::LogLevel::Info, __VA_ARGS__)
/**
 * @brief Logs an Info message without the calling function name, formatted using fmt::format
 */
#define LOGINF(...) LOGNF_WRITE(skyline::AsyncLogger::LogLevel::Info, __VA_ARGS__)

/**
 * @brief Logs a Debug message, formatted using fmt::format
 */
#define LOGD(...) LOG_WRITE(skyline::AsyncLogger::LogLevel::Debug, __VA_ARGS__)
/**
 * @brief Logs a Debug message without the calling function name, formatted using fmt::format
 */
#define LOGDNF(...) LOGNF_WRITE(skyline::AsyncLogger::LogLevel::Debug, __VA_ARGS__)

/**
 * @brief Logs a Verbose message, formatted using fmt::format
 */
#define LOGV(...) LOG_WRITE(skyline::AsyncLogger::LogLevel::Verbose, __VA_ARGS__)
/**
 * @brief Logs a Verbose message without the calling function name, formatted using fmt::format
 */
#define LOGVNF(...) LOGNF_WRITE(skyline::AsyncLogger::LogLevel::Verbose, __VA_ARGS__)

// Trace shares the persisted Verbose value: existing Android preferences remain valid.
#define LOGT(...) LOGV(__VA_ARGS__)
