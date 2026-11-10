#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Isolated old nested-PDF to flat-PDF package ownership migration.
set -euo pipefail
(($# == 1)) || { echo 'usage: check-documentation-upgrade.sh PACKAGE' >&2; exit 2; }
((EUID == 0)) || { echo 'Run isolated dpkg integration test as root' >&2; exit 2; }
package=$(realpath "$1")
pkg=infiltrator-filesystem-ufs2
repo=Filesystem-UFS2
module=UFS2.so
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
image="$work/old"
system="$work/system"
mkdir -p "$image/DEBIAN" "$image/System/Filesystems" \
  "$image/System/Resources/Documentation/$repo"
cat > "$image/DEBIAN/control" <<EOF
Package: $pkg
Version: 0.0.0
Architecture: amd64
Maintainer: Fixture <fixture@example.invalid>
Description: isolated old documentation layout
EOF
printf 'old filesystem module' > "$image/System/Filesystems/$module"
printf '%%PDF-1.4 old manual' > "$image/System/Resources/Documentation/$repo/UFS.pdf"
dpkg-deb --root-owner-group --build "$image" "$work/old.deb" >/dev/null
dpkg --root "$system" --unpack "$work/old.deb" >/dev/null
printf 'personal document' > "$system/System/Resources/Documentation/Personal.pdf"
dpkg --root "$system" --unpack "$package" >/dev/null
flat="$system/System/Resources/Documentation/$repo.pdf"
test "$(head -c 5 "$flat")" = '%PDF-'
test ! -e "$system/System/Resources/Documentation/$repo"
test "$(cat "$system/System/Resources/Documentation/Personal.pdf")" = 'personal document'
owner=$(dpkg-query --admindir "$system/var/lib/dpkg" --search "/System/Resources/Documentation/$repo.pdf")
test "$owner" = "$pkg: /System/Resources/Documentation/$repo.pdf"
printf '%s: flat manual ownership and old folder removal passed\n' "$pkg"
