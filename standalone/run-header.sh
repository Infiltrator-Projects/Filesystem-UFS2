#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
if [ "$#" -gt 0 ] && { [ "$1" = '--help' ] || [ "$1" = '-h' ]; }; then
  echo 'UFS2 native source installer [--profile aggressive|portable] [--build-only --output DIRECTORY] [--system-package-mode]'
  exit 0
fi
command -v git >/dev/null 2>&1 || { echo 'git is required to fetch the pinned source' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
git init -q "$work/source"
git -C "$work/source" fetch --quiet --depth=1 https://github.com/Infiltrator-Projects/Filesystem-UFS2.git @SOURCE_REVISION@
git -C "$work/source" checkout --quiet --detach FETCH_HEAD
test "$(git -C "$work/source" rev-parse HEAD)" = '@SOURCE_REVISION@'
bash "$work/source/standalone/release.sh" install "$@"
