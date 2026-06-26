// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Optimized: firmware version tracking, accelerated boot, MediaTek memory trim on exit

#pragma once

#include <common/language.h>
#include "vfs/filesystem.h"
#include "loader/loader.h"
#include "services/serviceman.h"

namespace skyline::kernel {

    /**
     * @brief Firmware version as a structured triple (major.minor.micro)
     *        Matches the Nintendo Switch firmware versioning scheme.
     *        The latest supported firmware is 22.1.0.
     */
    struct FirmwareVersion {
        u8 major{};
        u8 minor{};
        u8 micro{};

        // Latest supported firmware (22.1.0)
        static constexpr u8 kLatestMajor{22};
        static constexpr u8 kLatestMinor{1};
        static constexpr u8 kLatestMicro{0};

        constexpr FirmwareVersion() = default;
        constexpr FirmwareVersion(u8 major, u8 minor, u8 micro)
            : major{major}, minor{minor}, micro{micro} {}

        /**
         * @brief Returns the latest known firmware version (22.1.0)
         */
        static constexpr FirmwareVersion Latest() {
            return {kLatestMajor, kLatestMinor, kLatestMicro};
        }

        /**
         * @brief Encodes the version as a 32-bit word compatible with
         *        the HOS GetFirmwareVersion system call result.
         *        Layout: [major:8][minor:8][micro:4][rev:4][...padding]
         */
        [[nodiscard]] constexpr u32 Encode() const {
            return (static_cast<u32>(major) << 24) |
                   (static_cast<u32>(minor) << 16) |
                   (static_cast<u32>(micro) << 8);
        }

        [[nodiscard]] constexpr bool operator>=(const FirmwareVersion &other) const {
            if (major != other.major) return major > other.major;
            if (minor != other.minor) return minor > other.minor;
            return micro >= other.micro;
        }

        [[nodiscard]] constexpr bool operator==(const FirmwareVersion &) const = default;
    };

    /**
     * @brief The OS class manages the interaction between the various Skyline components.
     *
     * Changes vs original:
     *  - Exposes FirmwareVersion so services can query it without re-parsing NACP.
     *  - Tracks the active ROM name/version for crash reporting.
     *  - Calls MemoryManager::Trim() after the process exits to reclaim RAM
     *    before returning to the launcher (important on low-RAM MediaTek devices).
     */
    class OS {
      public:
        std::string nativeLibraryPath;    //!< Full path to the app's native library directory
        std::string publicAppFilesPath;   //!< Full path to the app's public files directory
        std::string privateAppFilesPath;  //!< Full path to the app's private files directory
        std::string deviceTimeZone;       //!< Timezone name (e.g. "Europe/London")
        std::shared_ptr<vfs::FileSystem> assetFileSystem; //!< Filesystem for emulator assets (tzdata etc.)

        DeviceState state;
        service::ServiceManager serviceManager;

        // --- Runtime metadata set during Execute() ---
        FirmwareVersion firmwareVersion{FirmwareVersion::Latest()}; //!< Reported firmware version (default: 22.1.0)
        std::string activeGameName;    //!< Display name of the currently running game
        std::string activeGameVersion; //!< Version string of the currently running game

        /**
         * @param jvmManager  The JVM manager for JNI calls
         * @param settings    Emulator settings
         * @param publicAppFilesPath   Path to public app storage
         * @param privateAppFilesPath  Path to private app storage (keys/)
         * @param deviceTimeZone       Host timezone string
         * @param nativeLibraryPath    Path to .so libraries
         * @param assetFileSystem      VFS for internal assets
         * @param firmwareVersion      Firmware version to report to games (default: 22.1.0)
         */
        OS(
            std::shared_ptr<JvmManager> &jvmManager,
            std::shared_ptr<Settings> &settings,
            std::string publicAppFilesPath,
            std::string privateAppFilesPath,
            std::string nativeLibraryPath,
            std::string deviceTimeZone,
            std::shared_ptr<vfs::FileSystem> assetFileSystem
        );

        /**
         * @brief Executes a ROM file.
         *        Selects the appropriate loader, initialises the GPU, creates
         *        the HOS process and blocks until it exits.
         *        Calls MemoryManager::Trim() before returning to reclaim RAM.
         * @param romFd    File descriptor for the ROM
         * @param romType  Format of the ROM (NRO, NSO, NCA, NSP, XCI)
         */
        void Execute(int romFd, loader::RomFormat romType);
    };
}
