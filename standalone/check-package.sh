#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Qualify the installed UFS2 SO, PDF, and optional kernel module without Python.
set -euo pipefail
usage() { echo 'usage: check-package.sh PACKAGE --profile portable|aggressive [--kernel-release RELEASE]' >&2; exit 2; }
(($# >= 3)) || usage
package=$(realpath -- "$1"); shift
profile= kernel_release=
while (($#)); do
  case "$1" in
    --profile) (($# >= 2)) || usage; profile=$2; shift 2 ;;
    --kernel-release) (($# >= 2)) || usage; kernel_release=$2; shift 2 ;;
    *) usage ;;
  esac
done
[[ "$profile" == portable || "$profile" == aggressive ]] || usage
root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Resolve the checkout root explicitly if the CI runner executes via a wrapper.
if [[ ! -f "$root/standalone/config.json" ]]; then
  checkout=$(git rev-parse --show-toplevel 2>/dev/null || :)
  if [[ -f "$checkout/standalone/config.json" ]]; then root=$checkout; fi
fi
[[ -f "$root/standalone/config.json" ]] || {
  echo "Filesystem source configuration missing: script=${BASH_SOURCE[0]} root=$root cwd=$PWD" >&2
  exit 1
}
for cmd in jq dpkg-deb readelf nm sha256sum; do
  command -v "$cmd" >/dev/null || { echo "Missing build prerequisite: $cmd" >&2; exit 1; }
done
repo=$(jq -er '.repository' "$root/standalone/config.json")
module=$(jq -er '.module' "$root/standalone/config.json")
abi=$(jq -er '.abi' "$root/standalone/config.json")
[[ "$repo" =~ ^Filesystem-[A-Za-z0-9]+$ && "$module" =~ ^[A-Z0-9]+\.so$ &&
   "$abi" =~ ^infiltrator_filesystem_[a-z0-9_]+$ ]] || {
  echo 'Invalid filesystem identity' >&2; exit 1;
}
if [[ -f "$root/SOURCE-REVISION" ]]; then
  revision=$(tr -d '\r\n' < "$root/SOURCE-REVISION")
else
  revision=$(git -C "$root" rev-parse HEAD)
fi
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid source revision' >&2; exit 1; }
[[ "$(dpkg-deb -f "$package" Package)" == "infiltrator-${repo,,}" &&
   "$(dpkg-deb -f "$package" Architecture)" == amd64 &&
   "$(dpkg-deb -f "$package" X-Infiltrator-Native-Profile)" == "$profile" &&
   "$(dpkg-deb -f "$package" X-Infiltrator-Source-Revision)" == "$revision" ]] || {
  echo 'Debian package identity, profile or source revision mismatch' >&2; exit 1;
}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
dpkg-deb -x "$package" "$work"
so="$work/System/Filesystems/$module"
pdf="$work/System/Resources/Documentation/$repo.pdf"
[[ -f "$so" && ! -L "$so" && -f "$pdf" && ! -L "$pdf" &&
   "$(head -c 5 "$pdf")" == '%PDF-' ]] || {
  echo 'Missing valid regular filesystem SO or PDF' >&2; exit 1;
}
readelf -h "$so" | grep -Fq 'DYN (Shared object file)' || {
  echo 'Filesystem SO must be ELF DYN' >&2; exit 1;
}
readelf -h "$so" | grep -Fq 'Advanced Micro Devices X86-64' || {
  echo 'Filesystem SO must be ELF x86-64' >&2; exit 1;
}
nm -D --defined-only "$so" | awk -v symbol="$abi" '$NF == symbol { found=1 } END { exit !found }' || {
  echo "Required native Common ABI missing: $abi" >&2; exit 1;
}
while IFS= read -r needed; do
  [[ -n "$needed" ]] || continue
  case "$needed" in
    libc.so.6|libpthread.so.0|libm.so.6|ld-linux-x86-64.so.2|libcrypto.so.3|libsqlite3.so.0|libstdc++.so.6|libgcc_s.so.1) ;;
    *) echo "Unexpected filesystem runtime dependency: $needed" >&2; exit 1 ;;
  esac
done < <(readelf -d "$so" | sed -nE 's/.*\(NEEDED\).*Shared library: \[([^]]+)\].*/\1/p')
[[ "$(dpkg-deb -f "$package" X-Infiltrator-Resource-SHA256)" == "$(sha256sum "$so" | cut -d' ' -f1)" &&
   "$(dpkg-deb -f "$package" X-Infiltrator-PDF-SHA256)" == "$(sha256sum "$pdf" | cut -d' ' -f1)" ]] || {
  echo 'Package SO or manual checksum mismatch' >&2; exit 1;
}
expected=("System/Filesystems/$module" "System/Resources/Documentation/$repo.pdf")
if [[ -n "$kernel_release" ]]; then
  [[ "$kernel_release" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || usage
  id=${repo#Filesystem-}; id=${id,,}
  ko="$work/System/Kernel/Filesystems/$id.ko"
  [[ -f "$ko" && ! -L "$ko" ]] || { echo 'Missing kernel adapter' >&2; exit 1; }
  readelf -h "$ko" | grep -Fq 'REL (Relocatable file)' || {
    echo 'Kernel adapter must be ELF REL' >&2; exit 1;
  }
  readelf -h "$ko" | grep -Fq 'Advanced Micro Devices X86-64' || {
    echo 'Kernel adapter must be ELF x86-64' >&2; exit 1;
  }
  modinfo=$(readelf -W -p .modinfo "$ko")
  grep -Eq "name=$id([[:space:]]|$)" <<<"$modinfo" || {
    echo 'Kernel adapter name mismatch' >&2; exit 1;
  }
  grep -Eq "vermagic=$kernel_release([[:space:]]|$)" <<<"$modinfo" || {
    echo 'Kernel vermagic mismatch' >&2; exit 1;
  }
  [[ "$(dpkg-deb -f "$package" X-Infiltrator-Kernel-Release)" == "$kernel_release" &&
     "$(dpkg-deb -f "$package" X-Infiltrator-Kernel-SHA256)" == "$(sha256sum "$ko" | cut -d' ' -f1)" ]] || {
    echo 'Kernel provenance mismatch' >&2; exit 1;
  }
  expected+=("System/Kernel/Filesystems/$id.ko")
fi
mapfile -t actual < <(find "$work" \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort)
mapfile -t known < <(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)
[[ "${actual[*]}" == "${known[*]}" ]] || {
  echo 'Unexpected or missing filesystem package payload' >&2; exit 1;
}
echo "FILESYSTEM-PACKAGE-CONTENTS-PASS $repo $profile"
