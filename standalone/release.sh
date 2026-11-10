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
make_run() {
  local destination=$1 archive="$work/source.tar.gz"
  mkdir -p "$work/source"
  git -C "$root" rev-parse --git-dir >/dev/null || { echo 'Release needs a Git checkout' >&2; return 1; }
  git ls-files -z | while IFS= read -r -d '' member; do
    [[ $member == docs/UFS.pdf ]] && continue
    [[ -f $member && ! -L $member ]] || { echo "Invalid source archive member: $member" >&2; exit 1; }
    mkdir -p "$work/source/$(dirname "$member")"
    cp -- "$member" "$work/source/$member"
  done
  printf '%s\n' "$(revision)" > "$work/source/SOURCE-REVISION"
  tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -czf "$archive" -C "$work/source" .
  local digest
  digest=$(sha256sum "$archive" | cut -d' ' -f1)
  sed "s/@PAYLOAD_SHA256@/$digest/g" standalone/run-header.sh > "$destination"
  cat "$archive" >> "$destination"
  chmod 0755 "$destination"
}
release() {
  check_tools release
  mkdir -p dist
  test -z "$(find dist -mindepth 1 -maxdepth 1 -print -quit)" || {
    echo 'Refusing to mix release artifacts' >&2; exit 1;
  }
  build_deb portable "dist/$package"_"$version"_amd64.deb ''
  make_run "dist/filesystem-ufs2-$version-native-installer.run"
  test "$(find dist -maxdepth 1 -type f | wc -l)" = 2
}
bundle_kernel() {
  (($# == 3)) || { echo 'bundle-kernel PACKAGE KERNEL OUTPUT' >&2; exit 2; }
  local input=$1 ko=$2 output=$3
  test -f "$input" && test -f "$ko"
  test "$(dpkg-deb -f "$input" Package)" = "$package"
  test "$(dpkg-deb -f "$input" Version)" = "$version"
  test "$(dpkg-deb -f "$input" X-Infiltrator-Native-Profile)" = portable
  test "$(dpkg-deb -f "$input" X-Infiltrator-Source-Revision)" = "$(revision)"
  local stage="$work/kernel-bundle"
  mkdir -p "$stage"
  dpkg-deb --raw-extract "$input" "$stage"
  test ! -e "$stage/$kernel_path"
  mkdir -p "$stage/System/Kernel/Filesystems"
  install -m 0644 "$ko" "$stage/$kernel_path"
  local release=6.12.107
  kernel_verify "$stage/$kernel_path" "$release"
  {
    printf 'X-Infiltrator-Kernel-SHA256: %s\n' "$(sha256sum "$ko" | cut -d' ' -f1)"
    printf 'X-Infiltrator-Kernel-Release: %s\n' "$release"
  } >> "$stage/DEBIAN/control"
  mkdir -p "$(dirname "$output")"
  dpkg-deb --root-owner-group --build "$stage" "$output"
  bash standalone/check-package.sh "$output" --profile portable --kernel-release "$release"
}
install_native() {
  local profile=aggressive system_mode=0 build_only=0 output='' selected_kernel=''
  while (($#)); do
    case $1 in
      --profile) profile=$2; shift 2;;
      --kernel-build) selected_kernel=$2; shift 2;;
      --output) output=$2; shift 2;;
      --build-only) build_only=1; shift;;
      --system-package-mode) system_mode=1; shift;;
      --help|-h) echo 'UFS2 [--profile aggressive|portable] [--kernel-build PATH] [--build-only --output PATH]'; return 0;;
      *) echo "Unknown native installer option: $1" >&2; return 2;;
    esac
  done
  case $profile in aggressive|portable);;*) echo 'Unknown profile' >&2; return 2;; esac
  if ((build_only)) && [[ -z $output ]]; then
    echo 'Build-only requires --output DIRECTORY' >&2; return 2
  fi
  if ((system_mode)) && [[ -n $selected_kernel ]]; then
    echo 'OS package mode forbids manual --kernel-build' >&2; return 2
  fi
  check_tools install
  if ((!system_mode)); then
    if [[ -z $selected_kernel ]]; then selected_kernel="/lib/modules/$(uname -r)/build"; fi
    test -s "$selected_kernel/Module.symvers" || {
      echo 'Prepared running-kernel source and Module.symvers required' >&2; return 1;
    }
  fi
  local version_suffix=''
  if [[ $profile == aggressive ]]; then version_suffix='+native1'; fi
  local filename="$package"_"$version$version_suffix"_amd64.deb
  local target_dir="$work"
  if ((build_only)); then target_dir=$(realpath -m "$output"); fi
  mkdir -p "$target_dir"
  local target="$target_dir/$filename"
  build_deb "$profile" "$target" "$selected_kernel"
  if ((build_only)); then printf '%s\n' "$target"; return 0; fi
  if ((EUID == 0)); then dpkg -i "$target"; else sudo dpkg -i "$target"; fi
  local installed="/System/Filesystems/$module"
  local pdf="/System/Resources/Documentation/$repository.pdf"
  test -f "$installed" && test -f "$pdf"
  test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$(dpkg-query -W -f='${X-Infiltrator-Resource-SHA256}' "$package")"
  test "$(sha256sum "$pdf" | cut -d' ' -f1)" = "$(dpkg-query -W -f='${X-Infiltrator-PDF-SHA256}' "$package")"
  test "$(dpkg-query -W -f='${Version}' "$package")" = "$version$version_suffix"
  dpkg-query -S "$installed" | grep -F "$package:" >/dev/null
  dpkg-query -S "$pdf" | grep -F "$package:" >/dev/null
  echo "Installed and verified $installed"
}
(($# > 0)) || { echo 'usage: release.sh release|install|bundle-kernel' >&2; exit 2; }
action=$1; shift
case $action in
  release) (($# == 0)) || exit 2; release;;
  install) install_native "$@";;
  bundle-kernel) bundle_kernel "$@";;
  *) echo "Unknown UFS2 native action: $action" >&2; exit 2;;
esac
