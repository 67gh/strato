# Bundled CPU decoder, built with the same NDK/API as skyline.
include(ExternalProject)
find_program(VIDEO_MAKE NAMES gmake make REQUIRED)
find_program(VIDEO_PATCH NAMES patch REQUIRED)
set(VIDEO_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/video-ffmpeg")
set(VIDEO_INSTALL "${VIDEO_PREFIX}/install")
file(MAKE_DIRECTORY "${VIDEO_INSTALL}/include")
set(VIDEO_CROSS_ARGS "")
if(ANDROID)
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "The bundled NVDEC decoder currently targets arm64-v8a")
    endif()
    list(APPEND VIDEO_CROSS_ARGS --enable-cross-compile --target-os=android --arch=aarch64
         "--sysroot=${CMAKE_SYSROOT}" "--extra-cflags=--target=${CMAKE_C_COMPILER_TARGET}"
         "--extra-ldflags=--target=${CMAKE_C_COMPILER_TARGET}")
else()
    list(APPEND VIDEO_CROSS_ARGS --disable-x86asm)
endif()
ExternalProject_Add(strato_video_ffmpeg
    PREFIX "${VIDEO_PREFIX}"
    URL https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz
    URL_HASH SHA256=089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304
    PATCH_COMMAND "${VIDEO_PATCH}" -p1 --forward -i "${CMAKE_CURRENT_LIST_DIR}/../patches/ffmpeg/nvdec-decode-order.patch"
    CONFIGURE_COMMAND <SOURCE_DIR>/configure "--prefix=${VIDEO_INSTALL}"
        "--cc=${CMAKE_C_COMPILER}" "--cxx=${CMAKE_CXX_COMPILER}" "--ar=${CMAKE_AR}" "--ranlib=${CMAKE_RANLIB}"
        ${VIDEO_CROSS_ARGS}
        --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
        --enable-decoder=h264,vp8 --enable-pic --enable-static --disable-shared
        --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample
    BUILD_COMMAND "${VIDEO_MAKE}" -j2
    INSTALL_COMMAND "${VIDEO_MAKE}" install
    BUILD_BYPRODUCTS "${VIDEO_INSTALL}/lib/libavcodec.a" "${VIDEO_INSTALL}/lib/libavutil.a"
)
add_library(strato_avcodec STATIC IMPORTED)
set_target_properties(strato_avcodec PROPERTIES IMPORTED_LOCATION "${VIDEO_INSTALL}/lib/libavcodec.a"
    INTERFACE_INCLUDE_DIRECTORIES "${VIDEO_INSTALL}/include")
add_library(strato_avutil STATIC IMPORTED)
set_target_properties(strato_avutil PROPERTIES IMPORTED_LOCATION "${VIDEO_INSTALL}/lib/libavutil.a")
add_dependencies(strato_avcodec strato_video_ffmpeg)
add_dependencies(strato_avutil strato_video_ffmpeg)
