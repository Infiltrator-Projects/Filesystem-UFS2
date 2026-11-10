/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ufs_module_v1.h"
#include "ifs_ufs_probe_v1.h"
#include "ifs_ufs_volume_check_v1.h"
#include "ufs_native.h"
#include <infiltratr/filesystem_module_v2.h>
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
static int accepts_volume(const char *path)
{
    LdUfsSummary summary;
    char error[256] = {0};

    /* The shared UFS decoder recognises UFS1 and UFS2. This module must
     * claim only the UFS2 little-endian volumes it actually implements. */
    if (path == NULL ||
        ufs_read_summary(path, &summary, error, sizeof(error)) != 0)
        return -1;
    return summary.variant == LD_UFS_VARIANT_UFS2_LE ? 0 : -1;
}
static const IfsUfsProbeV1 probe = {
    IFS_UFS_PROBE_ABI_V1, sizeof(IfsUfsProbeV1), "ufs2", accepts_volume
};
const IfsUfsProbeV1 *infiltrator_filesystem_ufs_probe_v1(void)
{
    return &probe;
}

/*
 * Native Common V2 interface. Only the read-only CORE probe is qualified.
 * The legacy Defragmenter/fixture API stays for existing consumers.
 * Do not advertise mount, format, repair, kernel or firmware capabilities
 * until their actual Common V2 host-backed implementations are tested.
 */
static int common_v2_probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    LdUfsSummary summary;
    int result;
    if (variant == NULL)
        return IFS_FS_INVALID;
    *variant = 0U;
    result = ufs_read_summary_host_v2(host, &summary);
    if (result != IFS_FS_OK)
        return result;
    if (summary.variant != LD_UFS_VARIANT_UFS2_LE)
        return IFS_FS_NOT_FOUND;
    *variant = (uint32_t)summary.variant;
    return IFS_FS_OK;
}

static const IfsFsCoreOpsV1 common_v2_core = {
    .header = {IFS_FS_GROUP_ABI_V1, sizeof(IfsFsCoreOpsV1),
               IFS_FS_GROUP_CORE, 0U, IFS_FS_CORE_PROBE},
    .probe = common_v2_probe
};

static const IfsFsGroupHeaderV2 *common_v2_query_group(
    uint32_t group_id, uint32_t minimum, uint32_t maximum)
{
    return group_id == IFS_FS_GROUP_CORE &&
           minimum <= IFS_FS_GROUP_ABI_V1 &&
           maximum >= IFS_FS_GROUP_ABI_V1
        ? &common_v2_core.header : NULL;
}

static const IfsFsModuleApiV2 common_v2_api = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "ufs2",
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    .base_v1 = NULL,
    .query_group = common_v2_query_group
};

const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &common_v2_api;
}
