/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ufs_module_v1.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 const IfsUfsModuleV1 *m = infiltrator_filesystem_ufs_v1();
 if (!m || m->abi_version != IFS_UFS_MODULE_V1 ||
     m->structure_bytes < sizeof(*m) || strcmp(m->format_id,"ufs") ||
     m->capabilities != (IFS_UFS_CAP_RUN | IFS_UFS_CAP_TEST_MEDIA) ||
     !m->run || !m->format_test_media) return 1;
 return 0;
}
