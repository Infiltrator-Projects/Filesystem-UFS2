/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS2: strict Common V2 probe, read-only lifecycle, and volume query.
 * The independent UFS engine remains the only decoder for disk geometry.
 * No unqualified file operations, relocation, writes, or legacy ABI exports.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include "ufs_native.h"

__attribute__((used,section(".ifsmeta")))
static const char identity[] = "IFS-MODULE:ufs2:common2";

static int inspect(const IfsFsHostRuntimeV2 *host, LdUfsSummary *summary)
{
    int rc = ufs_read_summary_host_v2(host, summary);
    if (rc != IFS_FS_OK) return rc;
    if (summary->variant != LD_UFS_VARIANT_UFS2_LE &&
        summary->variant != LD_UFS_VARIANT_UFS2_BE)
        return IFS_FS_NOT_FOUND;
    return IFS_FS_OK;
}

static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    LdUfsSummary summary;
    int rc;
    if (!variant) return IFS_FS_INVALID;
    *variant = 0u;
    rc = inspect(host, &summary);
    if (rc == IFS_FS_OK) *variant = (uint32_t)summary.variant;
    return rc;
}

/* This is a strictly read-only geometry session, not a writable filesystem
 * mount. The superblock must have validated allocation/cylinder geometry;
 * recognising a four-byte UFS magic alone is NOT enough for volume query.
 * Generation tokens reject stale callers without dereferencing freed memory.
 */
static struct {
    uintptr_t generation, active;
    IfsFsVolumeInfoV2 info;
} session;

static int live(void *volume)
{
    return session.active != 0u &&
           (uintptr_t)volume == session.active;
}

static int describe(const LdUfsSummary *s, uint64_t media_bytes,
                    IfsFsVolumeInfoV2 *info)
{
    uint64_t free_fragments, total_bytes, free_bytes;
    if (!s->allocation_totals_known || !s->cylinder_geometry_known ||
        !s->block_size || !s->fragment_size || !s->fragments_per_block ||
        !s->filesystem_fragments)
        return IFS_FS_UNSUPPORTED;
    if (s->filesystem_fragments > UINT64_MAX / s->fragment_size ||
        s->free_blocks > (UINT64_MAX - s->free_fragments) /
                         s->fragments_per_block)
        return IFS_FS_CORRUPT;
    free_fragments = s->free_blocks * s->fragments_per_block +
                     s->free_fragments;
    total_bytes = s->filesystem_fragments * s->fragment_size;
    if (free_fragments > s->filesystem_fragments ||
        total_bytes > media_bytes)
        return IFS_FS_CORRUPT;
    free_bytes = free_fragments * s->fragment_size;
    /* UFS names are at most 255 bytes; recorded superblock counts are
     * advisory, not an independently verified free-fragment scan. */
    info->structure_bytes = sizeof(*info);
    info->variant = (uint32_t)s->variant;
    info->max_name_bytes = 255u;
    info->block_bytes = s->block_size;
    info->total_bytes = total_bytes;
    info->free_bytes = free_bytes;
    return IFS_FS_OK;
}

static int mount_volume(const IfsFsHostRuntimeV2 *host, void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    LdUfsSummary summary;
    IfsFsVolumeInfoV2 measured = {0};
    int rc;
    if (!volume || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *volume = NULL;
    if (!ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    if (session.active || session.generation == UINTPTR_MAX)
        return IFS_FS_BUSY;
    rc = inspect(host, &summary);
    if (rc != IFS_FS_OK) return rc;
    rc = describe(&summary, host->volume_bytes, &measured);
    if (rc != IFS_FS_OK) return rc;
    session.info = measured;
    session.active = ++session.generation;
    *info = measured;
    *volume = (void *)session.active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if (!live(volume)) return IFS_FS_INVALID;
    session.active = 0u;
    session.info = (IfsFsVolumeInfoV2){0};
    return IFS_FS_OK;
}
static int query_volume(void *volume, IfsFsVolumeInfoV2 *info)
{
    if (!live(volume) || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *info = session.info;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE, 0u,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT},
    .probe = probe,
    .mount = mount_volume,
    .unmount = unmount_volume
};
static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY},
    .query = query_volume
};
static const IfsFsGroupHeaderV2 *query(uint32_t group_id,
                                       uint32_t minimum, uint32_t maximum)
{
    if (minimum > IFS_FS_GROUP_ABI_V2 ||
        maximum < IFS_FS_GROUP_ABI_V2) return NULL;
    if (group_id == IFS_FS_GROUP_CORE) return &core.header;
    if (group_id == IFS_FS_GROUP_VOLUME) return &volume_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "ufs2",
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group = query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
