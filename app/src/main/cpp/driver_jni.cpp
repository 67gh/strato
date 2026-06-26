// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Extended: Mali (MediaTek/Exynos) custom driver loading via direct dlopen

#include <jni.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vulkan/vulkan_raii.hpp>
#include <adrenotools/driver.h>
#include "skyline/common/format.h"
#include "skyline/common/signal.h"
#include "skyline/common/utils.h"

// ============================================================================
// GPU vendor detection helpers
// ============================================================================

/**
 * @brief Returns true if the Qualcomm KGSL device node is present.
 *        This is required for adrenotools-based custom driver loading.
 */
static bool CheckKgslPresent() {
    return access("/dev/kgsl-3d0", F_OK) == 0;
}

/**
 * @brief Returns true if an ARM Mali device node is present.
 *        Mali uses /dev/mali0 (or mali1, etc.) on MediaTek and Exynos SoCs.
 */
static bool CheckMaliPresent() {
    // Mali device nodes: /dev/mali0 through /dev/mali3
    for (int i = 0; i < 4; ++i) {
        auto path = fmt::format("/dev/mali{}", i);
        if (access(path.c_str(), F_OK) == 0)
            return true;
    }
    return false;
}

/**
 * @brief Returns true if any supported custom driver backend is available.
 *        This covers both Adreno (adrenotools) and Mali (direct dlopen).
 */
static bool CheckCustomDriverSupport() {
    return CheckKgslPresent() || CheckMaliPresent();
}

// ============================================================================
// Mali driver loading
// ============================================================================

/**
 * @brief Tries to load a custom Vulkan driver .so from the given directory
 *        using direct dlopen (no adrenotools required — works on Mali).
 *
 * @param driverDir   Full path to the directory containing the driver .so
 * @param libraryName The .so filename (e.g. "vulkan.mali.so")
 * @return dlopen handle on success, nullptr on failure
 */
static void *LoadMaliCustomDriver(const std::string &driverDir, const std::string &libraryName) {
    // Build the full path to the .so
    std::string fullPath = driverDir + libraryName;

    // Verify the file exists before trying to dlopen it
    struct stat st{};
    if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        LOGW("Mali custom driver not found at: {}", fullPath);
        return nullptr;
    }

    // dlopen with RTLD_NOW | RTLD_LOCAL to avoid polluting the global namespace
    void *handle = dlopen(fullPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        LOGW("Mali dlopen failed for {}: {}", fullPath, err ? err : "(unknown)");
        return nullptr;
    }

    // Validate that the .so actually exports vkGetInstanceProcAddr
    if (!dlsym(handle, "vkGetInstanceProcAddr")) {
        LOGW("Mali driver {} does not export vkGetInstanceProcAddr — not a valid Vulkan driver", libraryName);
        dlclose(handle);
        return nullptr;
    }

    LOGI("Mali custom driver loaded: {}", fullPath);
    return handle;
}

// ============================================================================
// JNI exports — system driver info
// ============================================================================

extern "C" JNIEXPORT jobjectArray JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_getSystemDriverInfo(JNIEnv *env, jobject) {
    auto libvulkanHandle{dlopen("libvulkan.so", RTLD_NOW)};

    vk::raii::Context vkContext{reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkanHandle, "vkGetInstanceProcAddr"))};
    vk::raii::Instance vkInstance{vkContext, vk::InstanceCreateInfo{}};
    vk::raii::PhysicalDevice physicalDevice{std::move(vk::raii::PhysicalDevices(vkInstance).front())};

    auto deviceProperties2{physicalDevice.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>()};
    auto properties{deviceProperties2.get<vk::PhysicalDeviceProperties2>().properties};

    auto driverId{vk::to_string(deviceProperties2.get<vk::PhysicalDeviceDriverProperties>().driverID)};
    auto driverVersion{fmt::format("{}.{}.{}",
        VK_API_VERSION_MAJOR(properties.driverVersion),
        VK_API_VERSION_MINOR(properties.driverVersion),
        VK_API_VERSION_PATCH(properties.driverVersion))};

    auto array = env->NewObjectArray(2, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(array, 0, env->NewStringUTF(driverId.c_str()));
    env->SetObjectArrayElement(array, 1, env->NewStringUTF(driverVersion.c_str()));

    return array;
}

// ============================================================================
// JNI exports — capability queries
// ============================================================================

extern "C" JNIEXPORT jboolean JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_supportsCustomDriverLoading(JNIEnv *, jobject) {
    // Now returns true for both Adreno (KGSL) and Mali devices
    return CheckCustomDriverSupport();
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_supportsForceMaxGpuClocks(JNIEnv *, jobject) {
    // Turbo mode via adrenotools is Adreno-only
    return CheckKgslPresent();
}

extern "C" JNIEXPORT void JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_forceMaxGpuClocks(JNIEnv *, jobject, jboolean enable) {
    if (CheckKgslPresent())
        adrenotools_set_turbo(enable);
    // Mali: no equivalent public API — silently ignored
}

// ============================================================================
// JNI exports — Mali driver loading
// ============================================================================

/**
 * @brief Returns true if the device has a Mali GPU.
 *        Called from Kotlin to conditionally show the Mali driver picker UI.
 */
extern "C" JNIEXPORT jboolean JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_isMaliDevice(JNIEnv *, jobject) {
    return CheckMaliPresent();
}

/**
 * @brief Validates that a custom Mali Vulkan driver .so is loadable.
 *        Used by the driver install flow to verify a driver before committing it.
 *
 * @param driverDirJstring   Full path to the directory containing the .so
 * @param libraryNameJstring The .so filename
 * @return true if the driver loaded and exports vkGetInstanceProcAddr
 */
extern "C" JNIEXPORT jboolean JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_validateMaliDriver(
        JNIEnv *env, jobject,
        jstring driverDirJstring,
        jstring libraryNameJstring) {

    const char *dirRaw = env->GetStringUTFChars(driverDirJstring, nullptr);
    const char *libRaw = env->GetStringUTFChars(libraryNameJstring, nullptr);

    std::string driverDir(dirRaw);
    std::string libraryName(libRaw);

    env->ReleaseStringUTFChars(driverDirJstring, dirRaw);
    env->ReleaseStringUTFChars(libraryNameJstring, libRaw);

    // Ensure the path ends with /
    if (!driverDir.empty() && driverDir.back() != '/')
        driverDir += '/';

    void *handle = LoadMaliCustomDriver(driverDir, libraryName);
    if (handle) {
        dlclose(handle);
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

/**
 * @brief Loads a custom Vulkan driver for Mali and returns the
 *        vkGetInstanceProcAddr function pointer as a long.
 *        Called from gpu.cpp's LoadVulkanDriver() when Mali is detected
 *        and a custom driver is configured.
 *
 * @param driverDirJstring   Full path to the directory containing the .so
 * @param libraryNameJstring The .so filename
 * @return vkGetInstanceProcAddr pointer as jlong, or 0 on failure
 */
extern "C" JNIEXPORT jlong JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_loadMaliDriver(
        JNIEnv *env, jobject,
        jstring driverDirJstring,
        jstring libraryNameJstring) {

    const char *dirRaw = env->GetStringUTFChars(driverDirJstring, nullptr);
    const char *libRaw = env->GetStringUTFChars(libraryNameJstring, nullptr);

    std::string driverDir(dirRaw);
    std::string libraryName(libRaw);

    env->ReleaseStringUTFChars(driverDirJstring, dirRaw);
    env->ReleaseStringUTFChars(libraryNameJstring, libRaw);

    if (!driverDir.empty() && driverDir.back() != '/')
        driverDir += '/';

    void *handle = LoadMaliCustomDriver(driverDir, libraryName);
    if (!handle)
        return 0L;

    auto procAddr = reinterpret_cast<jlong>(dlsym(handle, "vkGetInstanceProcAddr"));
    if (!procAddr) {
        dlclose(handle);
        return 0L;
    }

    // Note: handle is intentionally not closed here — it must stay alive
    // for the lifetime of the Vulkan instance. It will be cleaned up when
    // the process exits (Android will reclaim it).
    return procAddr;
}
