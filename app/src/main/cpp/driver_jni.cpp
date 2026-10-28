// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <jni.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <android/log.h>
#include "skyline/gpu/driver_platform.h"
#include <vulkan/vulkan_raii.hpp>
#include <adrenotools/driver.h>
#include "skyline/common/format.h"
#include "skyline/common/signal.h"
#include "skyline/common/utils.h"

extern "C" JNIEXPORT jobjectArray JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_getSystemDriverInfo(JNIEnv *env, jobject) {
    auto libvulkanHandle{dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL)};
    if (!libvulkanHandle) {
        const auto error{dlerror()};
        __android_log_print(ANDROID_LOG_WARN, "StratoDriver", "System Vulkan unavailable: %s", error ? error : "unknown");
        return nullptr;
    }
    jobjectArray result{};
    try {
        auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkanHandle, "vkGetInstanceProcAddr"));
        if (proc) {
            vk::raii::Context context{proc};
            vk::raii::Instance instance{context, vk::InstanceCreateInfo{}};
            auto devices{vk::raii::PhysicalDevices(instance)};
            if (!devices.empty()) {
                // Core properties do not require enabling the driver-properties extension.
                auto properties{devices.front().getProperties()};
                auto version{fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(properties.driverVersion), VK_API_VERSION_MINOR(properties.driverVersion), VK_API_VERSION_PATCH(properties.driverVersion))};
                result = env->NewObjectArray(2, env->FindClass("java/lang/String"), nullptr);
                if (result) {
                    env->SetObjectArrayElement(result, 0, env->NewStringUTF(properties.deviceName.data()));
                    env->SetObjectArrayElement(result, 1, env->NewStringUTF(version.c_str()));
                }
            } else __android_log_print(ANDROID_LOG_WARN, "StratoDriver", "No system Vulkan device");
        } else __android_log_print(ANDROID_LOG_WARN, "StratoDriver", "System Vulkan entry point missing");
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_WARN, "StratoDriver", "System Vulkan query failed: %s", error.what());
    }
    dlclose(libvulkanHandle);
    return result;
}

static bool CheckKgslPresent() {
    constexpr auto KgslPath{"/dev/kgsl-3d0"};

    return access(KgslPath, F_OK) == 0;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_supportsCustomDriverLoading(JNIEnv *env, jobject instance) {
    return skyline::gpu::driver::Detect() != skyline::gpu::driver::Platform::Unsupported;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_supportsForceMaxGpuClocks(JNIEnv *env, jobject instance) {
    // If the KGSL device exists adrenotools can be used to set GPU turbo mode
    return CheckKgslPresent();
}

extern "C" JNIEXPORT void JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_forceMaxGpuClocks(JNIEnv *env, jobject instance, jboolean enable) {
    if (CheckKgslPresent()) adrenotools_set_turbo(enable);
}

extern "C" JNIEXPORT jstring JNICALL Java_org_stratoemu_strato_utils_GpuDriverHelper_00024Companion_customDriverPlatform(JNIEnv *env, jobject) {
    using namespace skyline::gpu::driver;
    const auto platform{Detect()};
    return env->NewStringUTF(platform == Platform::Mali ? "mali" : platform == Platform::Adreno ? "adreno" : "unsupported");
}
