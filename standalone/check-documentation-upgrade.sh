#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Isolated dpkg upgrade fixture; do not touch the running system's dpkg database.
set -euo pipefail
(($# == 1 && EUID == 0)) || {
  echo 'usage: sudo check-documentation-upgrade.sh PACKAGE' >&2; exit 2;
}
package=$(realpath -- "$1")
root=$(CDPATH='' cd -- "$(dirname -- "\${BASH_SOURCE[0]}")/.." && pwd)
repo=$(jq -er '.repository' "$root/standalone/config.json")
module=$(jq -er '.module' "$root/standalone/config.json")
pdf=$(jq -er '.pdf' "$root/standalone/config.json")
[[ "$repo" =~ ^Filesystem-[A-Za-z0-9]+$ && "$module" =~ ^[A-Z0-9]+\.so$ &&
   "$pdf" =~ ^docs/[A-Za-z0-9]+\.pdf$ ]] || exit 1
name="infiltrator-\${repo,,}"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
old="$work/old"; system="$work/system"
mkdir -p "$old/DEBIAN" "$old/System/Filesystems" \
  "$old/System/Resources/Documentation/$repo"
printf 'Package: %s\nVersion: 0.0.0\nArchitecture: amd64\nMaintainer: Fixture <fixture@example.invalid>\nDescription: isolated old documentation layout\n' "$name" > "$old/DEBIAN/control"
printf 'old filesystem module' > "$old/System/Filesystems/$module"
printf '%%PDF-1.4 old manual\n' > "$old/System/Resources/Documentation/$repo/\${pdf##*/}"
dpkg-deb --root-owner-group --build "$old" "$work/old.deb" >/dev/null
dpkg --root "$system" --unpack "$work/old.deb" >/dev/null
personal="$system/System/Resources/Documentation/Personal.pdf"
printf 'personal document' > "$personal"
dpkg --root "$system" --unpack "$package" >/dev/null
flat="$system/System/Resources/Documentation/$repo.pdf"
[[ -f "$flat" && "$(head -c 5 "$flat")" == '%PDF-' &&
   ! -e "$system/System/Resources/Documentation/$repo" &&
   "$(cat "$personal")" == 'personal document' ]] || {
  echo 'Nested-to-flat upgrade or personal manual preservation failed' >&2; exit 1;
}
owner=$(dpkg-query --admindir "$system/var/lib/dpkg" --search "/System/Resources/Documentation/$repo.pdf")
[[ "$owner" == "$name: /System/Resources/Documentation/$repo.pdf" ]] || {
  echo 'Flat manual ownership mismatch' >&2; exit 1;
}
echo "$name: isolated flat manual upgrade passed"
