#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify the exact installed UFS2.so/PDF/kernel package contract without Python.
set -euo pipefail
if (($# < 3)); then
  echo "usage: check-package.sh PACKAGE --profile portable|aggressive [--kernel-release VERSION]" >&2
  exit 2
fi
package=$1; shift
profile=''
kernel_release=''
while (($#)); do
  case $1 in
    --profile) profile=${2:?missing profile}; shift 2;;
    --kernel-release) kernel_release=${2:?missing kernel release}; shift 2;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done
case $profile in portable|aggressive);;*) echo 'Expected portable or aggressive profile' >&2; exit 2;; esac
repo=Filesystem-UFS2
module=UFS2.so
package_name=infiltrator-filesystem-ufs2
root=$(cd "$(dirname "$0")/.." && pwd)
revision=$(if [[ -s $root/SOURCE-REVISION ]]; then cat "$root/SOURCE-REVISION"; else git -C "$root" rev-parse HEAD; fi)
[[ $revision =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid source revision' >&2; exit 1; }
test "$(dpkg-deb -f "$package" Package)" = "$package_name"
test "$(dpkg-deb -f "$package" X-Infiltrator-Native-Profile)" = "$profile"
test "$(dpkg-deb -f "$package" X-Infiltrator-Source-Revision)" = "$revision"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
dpkg-deb -x "$package" "$work"
so="$work/System/Filesystems/$module"
pdf="$work/System/Resources/Documentation/$repo.pdf"
test -f "$so" && test ! -L "$so"
test -f "$pdf" && test ! -L "$pdf"
test "$(head -c 5 "$pdf")" = '%PDF-'
test "$(readelf -h "$so" | sed -n 's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*/\1/p')" = DYN
readelf -h "$so" | grep -F 'Advanced Micro Devices X86-64' >/dev/null
nm -D --defined-only "$so" | awk '{print $NF}' | grep -Fx 'infiltrator_filesystem_api_v2' >/dev/null
test "$(sha256sum "$so" | cut -d' ' -f1)" = "$(dpkg-deb -f "$package" X-Infiltrator-Resource-SHA256)"
test "$(sha256sum "$pdf" | cut -d' ' -f1)" = "$(dpkg-deb -f "$package" X-Infiltrator-PDF-SHA256)"
expected=2
if [[ -n $kernel_release ]]; then
  ko="$work/System/Kernel/Filesystems/ufs2.ko"
  test -f "$ko" && test ! -L "$ko"
  readelf -W -h "$ko" | grep -Eq '^[[:space:]]*Type:[[:space:]]*REL[[:space:]]'
  readelf -W -h "$ko" | grep -F 'Advanced Micro Devices X86-64' >/dev/null
  readelf -W -p .modinfo "$ko" | grep -Eq 'name=ufs2([[:space:]]|$)'
  readelf -W -p .modinfo "$ko" | grep -F "vermagic=$kernel_release " >/dev/null
  test "$(dpkg-deb -f "$package" X-Infiltrator-Kernel-Release)" = "$kernel_release"
  test "$(sha256sum "$ko" | cut -d' ' -f1)" = "$(dpkg-deb -f "$package" X-Infiltrator-Kernel-SHA256)"
  expected=3
fi
test -z "$(find "$work" -type l -print -quit)"
test "$(find "$work" -type f | wc -l)" -eq "$expected"
test -z "$(find "$work/System/Resources/Documentation" -mindepth 1 -type d -print -quit)"
printf 'FILESYSTEM-PACKAGE-CONTENTS-PASS %s %s\n' "$repo" "$profile"
