/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ufs_module_v1.h"
#include "ifs_ufs_volume_check_v1.h"
#include "ifs_ufs_payload_verifier_v1.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 const IfsUfsModuleV1 *m = infiltrator_filesystem_ufs_v1();
 if (!m || m->abi_version != IFS_UFS_MODULE_V1 ||
     m->structure_bytes < sizeof(*m) || strcmp(m->format_id,"ufs") ||
     m->capabilities != (IFS_UFS_CAP_RUN | IFS_UFS_CAP_TEST_MEDIA) ||
     !m->run || !m->format_test_media) return 1;
 const IfsUfsVolumeCheckV1 *v = infiltrator_filesystem_ufs_volume_check_v1();
 if (!v || v->abi_version != IFS_UFS_VOLUME_CHECK_ABI_V1 ||
     v->structure_bytes < sizeof(*v) || !v->format_id ||
     strcmp(v->format_id, "ufs") || !v->is_ufs2_little_endian ||
     v->is_ufs2_little_endian("/nonexistent-ufs2-test-volume") == 0)
     return 2;
 const IfsUfsPayloadVerifierV1 *p =
     infiltrator_filesystem_ufs_payload_verifier_v1();
 if (!p || p->abi_version != IFS_UFS_PAYLOAD_VERIFIER_ABI_V1 ||
     p->structure_bytes < sizeof(*p) || !p->format_id ||
     strcmp(p->format_id, "ufs") || !p->verify_payload ||
     p->verify_payload("/nonexistent-ufs2-test-volume", NULL, 0, NULL, 0) == 0)
     return 3;
 return 0;
}
