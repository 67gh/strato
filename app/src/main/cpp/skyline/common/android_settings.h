// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "settings.h"
#include "jvm.h"

namespace skyline {
    /**
     * @brief Handles settings on the android platform
     * @note Lifetime of this class must not exceed the one of the JNIEnv contained inside ktSettings
     */
    class AndroidSettings final : public Settings {
      private:
        KtSettings ktSettings;

      public:
        /**
         * @note Will construct the underlying KtSettings object in-place
         */
        AndroidSettings(JNIEnv *env, jobject settingsInstance) : ktSettings(env, settingsInstance) {
            Update();
        }

        /**
         * @note Will take ownership of the passed KtSettings object
         */
        AndroidSettings(KtSettings &&ktSettings) : ktSettings(std::move(ktSettings)) {
            Update();
        }

        void Update() override {
            isDocked = ktSettings.GetBool("isDocked");
            usernameValue = std::move(ktSettings.GetString("usernameValue"));
            profilePictureValue = ktSettings.GetString("profilePictureValue");
            systemLanguage = ktSettings.GetInt<skyline::language::SystemLanguage>("systemLanguage");
            systemRegion = ktSettings.GetInt<skyline::region::RegionCode>("systemRegion");
            isInternetEnabled = ktSettings.GetBool("isInternetEnabled");
            forceTripleBuffering = ktSettings.GetBool("forceTripleBuffering");
            disableFrameThrottling = ktSettings.GetBool("disableFrameThrottling");
            gpuDriver = ktSettings.GetString("gpuDriver");
            gpuDriverLibraryName = ktSettings.GetString("gpuDriverLibraryName");
            executorSlotCountScale = ktSettings.GetInt<u32>("executorSlotCountScale");
            executorFlushThreshold = ktSettings.GetInt<u32>("executorFlushThreshold");
            useDirectMemoryImport = ktSettings.GetBool("useDirectMemoryImport");
            forceMaxGpuClocks = ktSettings.GetBool("forceMaxGpuClocks");
            disableShaderCache = ktSettings.GetBool("disableShaderCache");
            freeGuestTextureMemory = ktSettings.GetBool("freeGuestTextureMemory");
            enableFastGpuReadbackHack = ktSettings.GetBool("enableFastGpuReadbackHack");
            enableFastReadbackWrites = ktSettings.GetBool("enableFastReadbackWrites");
            disableSubgroupShuffle = ktSettings.GetBool("disableSubgroupShuffle");
            isAudioOutputDisabled = ktSettings.GetBool("isAudioOutputDisabled");
            logLevel = ktSettings.GetInt<skyline::AsyncLogger::LogLevel>("logLevel");
            validationLayer = ktSettings.GetBool("validationLayer");

            // Render scale is stored as a String (from a ListPreference: "0.5", "0.75", "1.0", "1.5", "2.0")
            try {
                renderScaleFactor = std::stof(std::string(ktSettings.GetString("renderScaleFactor")));
            } catch (...) {
                renderScaleFactor = 1.0f;
            }

            // Performance mode is stored as a String (ListPreference): "0", "1", or "2"
            try {
                performanceMode = static_cast<u32>(std::stoul(std::string(ktSettings.GetString("performanceMode"))));
            } catch (...) {
                performanceMode = 0;
            }

            // Apply performance mode overrides last so they take priority over individual
            // toggles without altering how those toggles are read above.
            switch (*performanceMode) {
                case 1: // Extreme Compatibility — survive on the weakest possible GPU
                    executorSlotCountScale = 2;        // Fewer parallel GPU submissions, less memory pressure
                    executorFlushThreshold = 64;        // Flush often, smaller command buffers
                    freeGuestTextureMemory = true;      // Aggressively reclaim texture RAM
                    enableFastGpuReadbackHack = false;  // Correctness over speed
                    enableFastReadbackWrites = false;
                    disableSubgroupShuffle = true;      // Best Mali compatibility
                    forceTripleBuffering = true;        // Smooth presentation even at very low FPS
                    disableFrameThrottling = false;
                    forceMaxGpuClocks = false;
                    break;
                case 2: // Extreme Performance — maximise GPU throughput at any cost
                    executorFlushThreshold = 512;       // Batch more commands before flush
                    freeGuestTextureMemory = false;      // Keep textures resident, avoid re-upload stalls
                    enableFastGpuReadbackHack = true;
                    enableFastReadbackWrites = true;
                    disableSubgroupShuffle = false;
                    forceTripleBuffering = false;        // Double buffer, lower latency
                    disableFrameThrottling = true;       // No vsync cap
                    forceMaxGpuClocks = true;
                    break;
                default: // Normal — respect individual user toggles as read above
                    break;
            }
        };
    };
}
