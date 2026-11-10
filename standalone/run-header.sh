#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
if [ "$#" -gt 0 ] && { [ "$1" = '--help' ] || [ "$1" = '-h' ]; }; then
  echo 'UFS2 native source installer [--profile aggressive|portable] [--kernel-build DIRECTORY] [--build-only --output DIRECTORY] [--system-package-mode]'
  exit 0
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
line=$(awk '/^__IFS_SOURCE_PAYLOAD_BELOW__$/ {print NR + 1; exit}' "$0")
test -n "$line"
tail -n +"$line" "$0" > "$work/source.tar.gz"
printf '%s  %s\n' '@PAYLOAD_SHA256@' "$work/source.tar.gz" | sha256sum -c - >/dev/null
mkdir "$work/source"
tar -xzf "$work/source.tar.gz" -C "$work/source" --no-same-owner --no-same-permissions
bash "$work/source/standalone/release.sh" install "$@"
exit $?
__IFS_SOURCE_PAYLOAD_BELOW__
