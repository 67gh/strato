// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <string_view>
#include <unistd.h>
namespace skyline::gpu::driver {
    enum class Platform { Unsupported, Adreno, Mali };
    template<class Probe> Platform Detect(Probe exists) {
        if (exists("/dev/kgsl-3d0")) return Platform::Adreno;
        if (exists("/dev/mali0")) return Platform::Mali;
        return Platform::Unsupported;
    }
    inline Platform Detect() {
        return Detect([](const char *path) { return access(path, F_OK) == 0; });
    }
    inline bool SafeComponent(std::string_view value) {
        return !value.empty() && value != "." && value != ".." &&
            value.find_first_of("/\\") == std::string_view::npos && value.find('\0') == std::string_view::npos;
    }
}
