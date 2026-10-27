#!/usr/bin/env bash
# Run from any directory. Requires host ffmpeg (libx264/libvpx encoders), g++, cmake, make, patch.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
result=${1:-"$repo/build/video-tests"}
mkdir -p "$result"
result=$(cd "$result" && pwd)
# Build the exact embedded FFmpeg using the production CMake integration.
mkdir -p "$result/project"
cat > "$result/project/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.16)
project(VideoHostTests LANGUAGES C CXX)
include("$repo/app/cmake/VideoDecoder.cmake")
CMAKE
cmake -S "$result/project" -B "$result/ffmpeg-build"
cmake --build "$result/ffmpeg-build" --target strato_video_ffmpeg --parallel 2
ffmpeg_install="$result/ffmpeg-build/video-ffmpeg/install"
ffmpeg -hide_banner -loglevel error -f lavfi -i 'testsrc2=size=64x48:rate=6' -frames:v 12 -c:v libx264 -profile:v high -x264-params 'bframes=2:ref=3:keyint=12:aud=1:scenecut=0:weightp=0' -pix_fmt yuv420p -y "$result/input.h264"
ffmpeg -hide_banner -loglevel error -f lavfi -i 'testsrc2=size=64x48:rate=6' -frames:v 12 -c:v libvpx -pix_fmt yuv420p -y "$result/input.ivf"
ffmpeg -hide_banner -loglevel error -i "$result/input.h264" -f rawvideo -pix_fmt yuv420p -y "$result/expected_h264.yuv"
ffmpeg -hide_banner -loglevel error -i "$result/input.ivf" -f rawvideo -pix_fmt yuv420p -y "$result/expected_vp8.yuv"
python3 "$repo/tools/video_tests/prepare.py" "$result"
video="$repo/app/src/main/cpp/skyline/soc/host1x/video"
g++ -std=c++20 -Wall -Wextra -Wconversion -Werror -g -fsanitize=address,undefined -I"$video" -isystem "$ffmpeg_install/include" \
 "$repo/tools/video_tests/video_test.cpp" "$video/decoder.cpp" "$video/compositor.cpp" \
 "$ffmpeg_install/lib/libavcodec.a" "$ffmpeg_install/lib/libavutil.a" -pthread -lm -o "$result/video_test"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$result/video_test" "$result/h264.jobs" "$result/vp8.jobs"
g++ -std=c++20 -Wall -Wextra -Wconversion -Werror -g -fsanitize=address,undefined -I"$video" \
 "$repo/tools/video_tests/compositor_test.cpp" "$video/compositor.cpp" -o "$result/compositor_test"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$result/compositor_test"
python3 "$repo/tools/video_tests/adapters_test.py" "$result" "$ffmpeg_install"
python3 "$repo/tools/render_tests/host1x_safety.py"
