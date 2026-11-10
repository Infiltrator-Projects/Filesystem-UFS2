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

static int host_read(void *context, uint64_t offset, void *buffer, uint32_t bytes)
{
    const int fd = *(const int *)context;
    return pread(fd, buffer, bytes, (off_t)offset) == (ssize_t)bytes
        ? IFS_FS_OK : IFS_FS_IO_ERROR;
}
static void *host_allocate(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes);
}
static void host_release(void *context, void *ptr)
{
    (void)context;
    free(ptr);
}

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

 const IfsFsModuleApiV2 *native = infiltrator_filesystem_api_v2();
 if (!ifs_fs_api_v2_native_valid(native, "ufs2")) return 7;
 const IfsFsCoreOpsV1 *core = (const IfsFsCoreOpsV1 *)
     ifs_fs_group_v2(native, IFS_FS_GROUP_CORE);
 if (!core || core->header.capabilities != IFS_FS_CORE_PROBE ||
     !core->probe || core->mount || core->read || core->write) return 8;
 IfsFsHostRuntimeV2 host = {
     .abi_version = IFS_FS_HOST_ABI_V2,
     .structure_bytes = sizeof(IfsFsHostRuntimeV2),
     .capabilities = IFS_FS_HOST_READ_ONLY,
     .volume_bytes = 65536,
     .logical_sector_bytes = 512,
     .physical_sector_bytes = 512,
     .maximum_transfer_bytes = 4096,
     .context = &fd,
     .read_at = host_read,
     .allocate = host_allocate,
     .release = host_release
 };
 uint32_t variant = 0U;
 int v2_good = core->probe(&host, &variant) == IFS_FS_OK &&
     variant == LD_UFS_VARIANT_UFS2_LE;
 if (pwrite(fd, one, sizeof(one), magic) != (ssize_t)sizeof(one))
     v2_good = 0;
 if (core->probe(&host, &variant) != IFS_FS_NOT_FOUND)
     v2_good = 0;
 IfsFsHostRuntimeV2 invalid_host = host;
 invalid_host.read_at = NULL;
 if (core->probe(&invalid_host, &variant) != IFS_FS_INVALID)
     v2_good = 0;
 close(fd);
 unlink(image);
 return good && v2_good ? 0 : 6;
}
