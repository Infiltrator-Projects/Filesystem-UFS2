/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ufs_module_v1.h"
#include "ifs_ufs_probe_v1.h"
#include "ifs_ufs_volume_check_v1.h"
#include "ufs_native.h"
int ifs_ufs_worker_main(int argc, char **argv);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int run(int argc, char **argv, const IfsDefragServicesV1 *services) {
    if (ifs_bind_services(services) != 0) return 1;
    const int result = ifs_ufs_worker_main(argc, argv);
    ifs_unbind_services();
    return result;
}
int ifs_ufs_mkfs_main(int argc, char **argv);
static const IfsUfsModuleV1 api = {
    IFS_UFS_MODULE_V1, sizeof(IfsUfsModuleV1),
    "ufs2", IFS_UFS_CAP_RUN | IFS_UFS_CAP_TEST_MEDIA, run, ifs_ufs_mkfs_main
};
const IfsUfsModuleV1 *infiltrator_filesystem_ufs_v1(void) { return &api; }

/* UFS-specific interpretation stays in UFS.so, never in Test Media. */
static int ufs_is_ufs2_le(const char *path)
{
    LdUfsSummary summary;
    char error[256] = {0};
    if (!path || ufs_read_summary(path, &summary, error, sizeof(error)) != 0)
        return -1;
    return summary.variant == LD_UFS_VARIANT_UFS2_LE ? 0 : -1;
}
static const IfsUfsVolumeCheckV1 volume_check = {
    IFS_UFS_VOLUME_CHECK_ABI_V1, sizeof(IfsUfsVolumeCheckV1),
    "ufs2", ufs_is_ufs2_le
};
const IfsUfsVolumeCheckV1 *infiltrator_filesystem_ufs_volume_check_v1(void)
{
    return &volume_check;
}

/* Used by the format-neutral Defragmenter worker to select the correct SO. */
static int accepts_volume(const char *path) {
    return path && ufs_probe(path) ? 0 : -1;
}
static const IfsUfsProbeV1 probe = {
    IFS_UFS_PROBE_ABI_V1, sizeof(IfsUfsProbeV1), "ufs2", accepts_volume
};
const IfsUfsProbeV1 *infiltrator_filesystem_ufs_probe_v1(void)
{
    return &probe;
}
