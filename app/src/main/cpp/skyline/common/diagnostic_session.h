// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace skyline::diagnostics {
    namespace detail {
        // Configure once before the emulation threads start; the path is read-only during a run.
        inline std::string sessionDirectory;

        inline bool IsFileName(std::string_view name) noexcept {
            return !name.empty() && name != "." && name != ".." &&
                   name.find_first_of("/\\") == std::string_view::npos &&
                   name.find('\0') == std::string_view::npos;
        }
    }

    inline void Configure(std::string directory) noexcept {
        detail::sessionDirectory.clear();
        try {
            if (directory.empty())
                return;
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error || !std::filesystem::is_directory(directory, error) || error)
                return;
            if (directory.back() != '/')
                directory += '/';
            detail::sessionDirectory = std::move(directory);
        } catch (...) {
            // A diagnostic directory failure must not prevent the game from starting.
        }
    }

    inline std::string OutputPath(std::string_view name, std::string fallback) noexcept {
        try {
            if (!detail::sessionDirectory.empty() && detail::IsFileName(name))
                return detail::sessionDirectory + std::string{name};
        } catch (...) {
        }
        return fallback;
    }

    // Returns escaped JSON string contents, without surrounding quotation marks.
    inline std::string JsonEscape(std::string_view value) {
        constexpr char Hex[]{"0123456789abcdef"};
        std::string escaped;
        escaped.reserve(value.size());
        for (unsigned char character : value) {
            switch (character) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\b': escaped += "\\b"; break;
                case '\f': escaped += "\\f"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:
                    if (character < 0x20) {
                        escaped += "\\u00";
                        escaped += Hex[character >> 4];
                        escaped += Hex[character & 0xf];
                    } else {
                        escaped += static_cast<char>(character);
                    }
            }
        }
        return escaped;
    }

    inline bool WriteJson(std::string_view name, std::string_view contents) noexcept {
        int fd{-1};
        std::string temporary;
        try {
            auto path{OutputPath(name, {})};
            if (path.empty())
                return false;
            temporary = path + ".tmp";
            fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (fd < 0)
                return false;
            size_t offset{};
            while (offset < contents.size()) {
                auto count{write(fd, contents.data() + offset, contents.size() - offset)};
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0) {
                    close(fd);
                    fd = -1;
                    unlink(temporary.c_str());
                    return false;
                }
                offset += static_cast<size_t>(count);
            }
            auto closeResult{close(fd)};
            fd = -1;
            // Publish only complete JSON documents, including when export runs concurrently.
            if (closeResult == 0 && rename(temporary.c_str(), path.c_str()) == 0)
                return true;
        } catch (...) {
        }
        if (fd >= 0)
            close(fd);
        if (!temporary.empty())
            unlink(temporary.c_str());
        return false;
    }
}
