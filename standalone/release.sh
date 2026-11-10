#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# UFS2 source-owned package/release, independent of Python.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
package=infiltrator-filesystem-ufs2
module=UFS2.so
repository=Filesystem-UFS2
kernel_path=System/Kernel/Filesystems/ufs2.ko
version=$(tr -d '\r\n' < VERSION)
[[ $version =~ ^[0-9]+[.][0-9]+[.][0-9]+$ ]] || { echo 'Invalid VERSION' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
revision() {
  if [[ -s "$root/SOURCE-REVISION" ]]; then cat "$root/SOURCE-REVISION";
  else git -C "$root" rev-parse HEAD; fi
}
check_tools() {
  local missing=0 cmd
  for cmd in cc c++ make cmake ctest git readelf nm dpkg-deb dpkg-query pkg-config sha256sum tar gzip; do
    command -v "$cmd" >/dev/null 2>&1 || missing=1
  done
  for cmd in build-essential binutils cmake git pkg-config libssl-dev libsqlite3-dev dpkg-dev; do
    [[ $(dpkg-query -W -f='${Status}' "$cmd" 2>/dev/null || true) == 'install ok installed' ]] || missing=1
  done
  if ((missing)); then
    [[ $1 == install ]] || { echo 'Missing native UFS2 build prerequisites' >&2; exit 1; }
    if ((EUID != 0)); then sudo apt-get update; sudo apt-get install -y --no-install-recommends build-essential binutils cmake git pkg-config libssl-dev libsqlite3-dev dpkg-dev;
    else apt-get update; apt-get install -y --no-install-recommends build-essential binutils cmake git pkg-config libssl-dev libsqlite3-dev dpkg-dev; fi
  fi
}
runtime_deps() {
  local so=$1 lib result=libc6
  local seen=' '
  while IFS= read -r lib; do
    case $lib in
      libc.so.6|libpthread.so.0|libm.so.6|ld-linux-x86-64.so.2) ;;
      libcrypto.so.3) if [[ $seen != *" crypto "* ]]; then result="$result, libssl3t64 | libssl3";seen="$seen crypto ";fi;;
      libsqlite3.so.0) if [[ $seen != *" sqlite "* ]]; then result="$result, libsqlite3-0";seen="$seen sqlite ";fi;;
      libstdc++.so.6) if [[ $seen != *" stdcpp "* ]]; then result="$result, libstdc++6";seen="$seen stdcpp ";fi;;
      libgcc_s.so.1) if [[ $seen != *" gcc "* ]]; then result="$result, libgcc-s1";seen="$seen gcc ";fi;;
      *) echo "Unexpected UFS2 runtime library: $lib" >&2; return 1;;
    esac
  done < <(readelf -d "$so" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
  printf '%s\n' "$result"
}
kernel_release() {
  test -s "$1/include/generated/utsrelease.h" && test -s "$1/Module.symvers"
  sed -n 's/^#define UTS_RELEASE "\([^"]*\)"$/\1/p' "$1/include/generated/utsrelease.h"
}
kernel_verify() {
  test -f "$1" && test ! -L "$1"
  readelf -W -h "$1" | grep -Eq '^[[:space:]]*Type:[[:space:]]*REL[[:space:]]'
  readelf -W -h "$1" | grep -F 'Advanced Micro Devices X86-64' >/dev/null
  readelf -W -p .modinfo "$1" | grep -Eq 'name=ufs2([[:space:]]|$)'
  readelf -W -p .modinfo "$1" | grep -F "vermagic=$2 " >/dev/null
}
build_deb() {
  local profile=$1 destination=$2 prepared_kernel=$3
  local stage="$work/package-root"
  rm -rf "$stage"
  mkdir -p "$stage/System/Filesystems" "$stage/System/Resources/Documentation" "$stage/DEBIAN"
  local so="$stage/System/Filesystems/$module"
  bash standalone/build.sh --profile "$profile" --output "$so"
  c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror standalone/manual.cpp -o "$work/ufs2-manual"
  local pdf="$stage/System/Resources/Documentation/$repository.pdf"
  "$work/ufs2-manual" docs/UFS.pages "$pdf" "$version"
  test "$(head -c 5 "$pdf")" = '%PDF-'
  local source_hash=$(revision)
  [[ $source_hash =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid source revision' >&2; return 1; }
  local flags='-O2 -march=x86-64 -mtune=generic' pgo=disabled lto=disabled deb_version="$version"
  if [[ $profile == aggressive ]]; then
    flags='-O3 -march=native -mtune=native -flto -fprofile-use'
    pgo=trained-two-pass
    lto=enabled
    deb_version="$version+native1"
  fi
  {
    printf 'Package: %s\nVersion: %s\nArchitecture: amd64\n' "$package" "$deb_version"
    printf 'Section: admin\nPriority: optional\n'
    printf 'Maintainer: Infiltrator Projects <noreply@infiltrator-projects.invalid>\n'
    printf 'Depends: %s\n' "$(runtime_deps "$so")"
    printf 'X-Infiltrator-Source-Revision: %s\n' "$source_hash"
    printf 'X-Infiltrator-Native-Profile: %s\n' "$profile"
    printf 'X-Infiltrator-LTO: %s\n' "$lto"
    printf 'X-Infiltrator-PGO: %s\n' "$pgo"
    printf 'X-Infiltrator-Cflags: %s\n' "$flags"
    printf 'X-Infiltrator-Resource-SHA256: %s\n' "$(sha256sum "$so" | cut -d' ' -f1)"
    printf 'X-Infiltrator-PDF-SHA256: %s\n' "$(sha256sum "$pdf" | cut -d' ' -f1)"
  } > "$stage/DEBIAN/control"
  local krel=''
  if [[ -n $prepared_kernel ]]; then
    local ko="$stage/$kernel_path"
    mkdir -p "$(dirname "$ko")"
    bash standalone/kernel.sh "$ko" "$prepared_kernel"
    krel=$(kernel_release "$prepared_kernel")
    kernel_verify "$ko" "$krel"
    {
      printf 'X-Infiltrator-Kernel-SHA256: %s\n' "$(sha256sum "$ko" | cut -d' ' -f1)"
      printf 'X-Infiltrator-Kernel-Release: %s\n' "$krel"
    } >> "$stage/DEBIAN/control"
  fi
  {
    printf 'Description: Independent UFS2 filesystem shared object for InfiltratorOS\n'
    printf ' Installs UFS2.so and its illustrated PDF.\n'
    if [[ -n $prepared_kernel ]]; then printf ' Includes the Linux kernel module for manual use.\n'; fi
  } >> "$stage/DEBIAN/control"
  mkdir -p "$(dirname "$destination")"
  dpkg-deb --root-owner-group --build "$stage" "$destination"
  if [[ -n $krel ]]; then
    bash standalone/check-package.sh "$destination" --profile "$profile" --kernel-release "$krel"
  else bash standalone/check-package.sh "$destination" --profile "$profile"; fi
}
