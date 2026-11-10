#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build exactly the local independent UFS2 Linux adapter.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
(($# >= 1 && $# <= 2)) || { echo 'usage: kernel.sh OUTPUT_KO [PREPARED_KERNEL_DIR]' >&2; exit 2; }
destination=$(realpath -m "$1")
release=$(uname -r)
build="/lib/modules/$release/build"
if (($# == 2)); then build=$2; fi
build=$(realpath "$build")
header="$build/include/generated/utsrelease.h"
test -s "$header" && test -s "$build/Module.symvers" || {
  echo "Kernel headers with Module.symvers required at $build" >&2; exit 1;
}
qualified=$(sed -n 's/^#define UTS_RELEASE "\([^"]*\)"$/\1/p' "$header")
[[ $qualified =~ ^[A-Za-z0-9.+_~-]+$ ]] || { echo 'Invalid kernel release header' >&2; exit 1; }
if (($# == 1)) && [[ $qualified != "$release" ]]; then
  echo "Running kernel differs from build tree" >&2; exit 1
fi
src="$root/native/filesystems/ufs"
test -f "$src/linux/Makefile" || { echo 'Local independent kernel source missing' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/native/filesystems"
cp -a "$src" "$work/native/filesystems/ufs"
module_dir="$work/native/filesystems/ufs/linux"
jobs=2
if [[ -n "$(printenv FILESYSTEM_KERNEL_JOBS 2>/dev/null || true)" ]]; then jobs=$(printenv FILESYSTEM_KERNEL_JOBS); fi
[[ $jobs =~ ^[1-9][0-9]*$ ]] || exit 2
make -C "$build" "M=$module_dir" -j"$jobs" CONFIG_UFS_FS=m modules
module="$module_dir/ufs2.ko"
test -s "$module"
readelf -W -h "$module" | grep -Eq '^[[:space:]]*Class:[[:space:]]*ELF64[[:space:]]*$'
readelf -W -h "$module" | grep -Eq '^[[:space:]]*Type:[[:space:]]*REL[[:space:]]'
readelf -W -h "$module" | grep -F 'Advanced Micro Devices X86-64' >/dev/null
readelf -W -p .modinfo "$module" | grep -Eq 'name=ufs2([[:space:]]|$)'
readelf -W -p .modinfo "$module" | grep -F "vermagic=$qualified " >/dev/null
mkdir -p "$(dirname "$destination")"
install -m 0644 "$module" "$destination"
echo "QUALIFIED UFS2 kernel adapter $qualified"
