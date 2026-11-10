#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Source-owned UFS2 native build and qualification. No Python.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
profile=portable
output="$root/build/UFS2.so"
while (($#)); do
  case $1 in
    --profile) profile=$2; shift 2;;
    --output) output=$2; shift 2;;
    --check) shift;;
    *) echo "Unknown UFS2 build argument: $1" >&2; exit 2;;
  esac
done
case "$profile" in portable|aggressive);;*) echo 'Unknown UFS2 profile' >&2; exit 2;; esac
for executable in cc c++ cmake ctest make git readelf nm dpkg-deb sha256sum; do
  if ! command -v "$executable" >/dev/null 2>&1; then
    echo "Missing native build prerequisite: $executable" >&2
    exit 1
  fi
done
output=$(realpath -m "$output")
mkdir -p "$(dirname "$output")"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
dir="$work/build"
flags='-O2 -march=x86-64 -mtune=generic -DNDEBUG'
if [[ $profile == aggressive ]]; then
  flags='-O3 -march=native -mtune=native -flto -DNDEBUG'
  mkdir -p "$work/profiles"
  generate="-fprofile-generate=$work/profiles"
  cmake -S "$root" -B "$dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS_RELEASE=$flags $generate" \
    "-DCMAKE_CXX_FLAGS_RELEASE=$flags $generate" \
    "-DCMAKE_SHARED_LINKER_FLAGS=$generate"
  cmake --build "$dir" --parallel 2
  ctest --test-dir "$dir" --output-on-failure
  if ! find "$work/profiles" -type f -name '*.gcda' -size +0c -print -quit | grep -q .; then
    echo 'Native two-pass PGO did not exercise UFS2 code' >&2; exit 1
  fi
  cmake -S "$root" -B "$dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS_RELEASE=$flags -fprofile-use=$work/profiles -fprofile-correction -Wno-missing-profile" \
    "-DCMAKE_CXX_FLAGS_RELEASE=$flags -fprofile-use=$work/profiles -fprofile-correction -Wno-missing-profile" \
    "-DCMAKE_SHARED_LINKER_FLAGS=-fprofile-use=$work/profiles"
  cmake --build "$dir" --clean-first --parallel 2
  ctest --test-dir "$dir" --output-on-failure
else
  cmake -S "$root" -B "$dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS_RELEASE=$flags" "-DCMAKE_CXX_FLAGS_RELEASE=$flags"
  cmake --build "$dir" --parallel 2
  ctest --test-dir "$dir" --output-on-failure
fi
module="$dir/UFS2.so"
test -s "$module" && test ! -L "$module"
readelf -h "$module" | grep -F 'DYN (Shared object file)' >/dev/null
readelf -h "$module" | grep -F 'Advanced Micro Devices X86-64' >/dev/null
nm -D --defined-only "$module" | awk '{print $NF}' | grep -Fx 'infiltrator_filesystem_api_v2' >/dev/null
install -m 0644 "$module" "$output"
test "$(sha256sum "$module" | cut -d' ' -f1)" = "$(sha256sum "$output" | cut -d' ' -f1)"
printf 'QUALIFIED UFS2.so %s %s\n' "$profile" "$flags"
