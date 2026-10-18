#!/usr/bin/env bash
set -euo pipefail
fixture_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(cd -- "$fixture_dir/../.." && pwd)"
fmt_include="${FMT_INCLUDE:-$project_dir/app/libraries/fmt/include}"
if [[ ! -f "$fmt_include/fmt/format.h" ]]; then
  printf '%s\n' 'Initialiser fmt ou fournir FMT_INCLUDE=/chemin/vers/fmt/include' >&2
  exit 1
fi
build_dir="$(mktemp -d)"
src_dir="$project_dir/app/src/main/cpp/skyline"
flags=(-std=c++20 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
g++ "${flags[@]}" -DFMT_HEADER_ONLY -I"$fixture_dir/stubs" -I"$src_dir" -I"$fmt_include" "$fixture_dir/test_runtime.cpp" "$src_dir/logger/logger.cpp" "$src_dir/common/diagnostic_recorder.cpp" "$src_dir/common/diagnostic_crash.cpp" "$src_dir/gpu/diagnostics.cpp" -o "$build_dir/runtime"
ASAN_OPTIONS=detect_leaks=0 timeout 30s "$build_dir/runtime" "$build_dir/session"
for name in containers memory stub; do
  g++ "${flags[@]}" "$fixture_dir/test_$name.cpp" -o "$build_dir/$name"
  ASAN_OPTIONS=detect_leaks=0 timeout 30s "$build_dir/$name"
done
printf 'Sorties de test : %s\n' "$build_dir"
