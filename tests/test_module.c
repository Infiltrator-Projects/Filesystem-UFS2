/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ufs_module_v1.h"
#include "ifs_ufs_probe_v1.h"
#include "ifs_ufs_volume_check_v1.h"
#include "ifs_ufs_payload_verifier_v1.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
 const IfsUfsModuleV1 *m = infiltrator_filesystem_ufs_v1();
 if (!m || m->abi_version != IFS_UFS_MODULE_V1 ||
     m->structure_bytes < sizeof(*m) || strcmp(m->format_id,"ufs2") ||
     m->capabilities != (IFS_UFS_CAP_RUN | IFS_UFS_CAP_TEST_MEDIA) ||
     !m->run || !m->format_test_media) return 1;
 const IfsUfsVolumeCheckV1 *v = infiltrator_filesystem_ufs_volume_check_v1();
 if (!v || v->abi_version != IFS_UFS_VOLUME_CHECK_ABI_V1 ||
     v->structure_bytes < sizeof(*v) || !v->format_id ||
     strcmp(v->format_id, "ufs2") || !v->is_ufs2_little_endian ||
     v->is_ufs2_little_endian("/nonexistent-ufs2-test-volume") == 0)
     return 2;
 const IfsUfsPayloadVerifierV1 *p =
     infiltrator_filesystem_ufs_payload_verifier_v1();
 if (!p || p->abi_version != IFS_UFS_PAYLOAD_VERIFIER_ABI_V1 ||
     p->structure_bytes < sizeof(*p) || !p->format_id ||
     strcmp(p->format_id, "ufs2") || !p->verify_payload ||
     p->verify_payload("/nonexistent-ufs2-test-volume", NULL, 0, NULL, 0) == 0)
     return 3;
 const IfsUfsProbeV1 *q = infiltrator_filesystem_ufs_probe_v1();
 if (!q || q->abi_version != IFS_UFS_PROBE_ABI_V1 ||
     q->structure_bytes < sizeof(*q) || !q->format_id ||
     strcmp(q->format_id, "ufs2") || !q->accepts_volume ||
     q->accepts_volume("/nonexistent-ufs2-test-volume") == 0)
     return 4;
 char image[] = "/tmp/ufs2-only-test-XXXXXX";
 int fd = mkstemp(image);
 if (fd < 0) return 5;
 const unsigned char one[4] = {0x54, 0x19, 0x01, 0x00};
 const unsigned char two[4] = {0x19, 0x01, 0x54, 0x19};
 const off_t magic = 8192 + 1372;
 int good = ftruncate(fd, 65536) == 0 &&
     pwrite(fd, two, sizeof(two), magic) == (ssize_t)sizeof(two) &&
     q->accepts_volume(image) == 0 &&
     pwrite(fd, one, sizeof(one), magic) == (ssize_t)sizeof(one) &&
     q->accepts_volume(image) != 0;
 close(fd);
 unlink(image);
 return good ? 0 : 6;
}
