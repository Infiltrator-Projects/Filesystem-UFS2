#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
so=${1:?UFS2.so required}
test -s "$so"
test "$(nm -D --defined-only "$so" |
  awk '$3=="infiltrator_filesystem_api_v2"{n++}END{print n+0}')" -eq 1
other="$(nm -D --defined-only "$so" | awk '{print $3}' |
  sed -E 's/@@?.*$//' |
  grep -v '^infiltrator_filesystem_api_v2$' || true)"
test -z "$other" || { echo "Unexpected UFS2 exports: $other" >&2;exit 1; }
if nm -D "$so" | grep -E ' U (ifs_bind_services|ifs_unbind_services|ld_pread_full|ld_pwrite_full|ld_stop_requested|ld_bitmap_size)($|@)'; then
  echo 'Production UFS2 depends on retired V1 service API' >&2
  exit 1
fi
