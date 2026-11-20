/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS2: strict Common V2 probe, read-only lifecycle, and volume query.
 * The independent UFS engine remains the only decoder for disk geometry.
 * No unqualified file operations, relocation, writes, or legacy ABI exports.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include "ufs_native.h"

__attribute__((used,retain,section(".ifsmeta")))
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
#define UFS2_MAX_SESSIONS 32u
/* The provider is shared across mounted volumes. A monotonically increasing
 * opaque token identifies a mount, never an address of a reusable slot.
 * Previously unmounted tokens cannot become valid again. The host must
 * serialize mount/unmount and operations on a session. */
typedef struct {
    uintptr_t active;
    IfsFsVolumeInfoV2 info;
    const IfsFsHostRuntimeV2 *host;
    LdUfsSummary summary;
    void *map_inventory;
} Ufs2Session;
static Ufs2Session sessions[UFS2_MAX_SESSIONS];
static uintptr_t session_sequence;

static Ufs2Session *find_session(void *volume)
{
    const uintptr_t token = (uintptr_t)volume;
    if (!token) return NULL;
    for (unsigned i = 0u; i < UFS2_MAX_SESSIONS; ++i)
        if (sessions[i].active == token) return &sessions[i];
    return NULL;
}
static Ufs2Session *unused_session(void)
{
    for (unsigned i = 0u; i < UFS2_MAX_SESSIONS; ++i)
        if (!sessions[i].active) return &sessions[i];
    return NULL;
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
    Ufs2Session *slot;
    int rc;
    if (!volume || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *volume = NULL;
    if (!ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    slot = unused_session();
    if (!slot || session_sequence == UINTPTR_MAX)
        return IFS_FS_BUSY;
    rc = inspect(host, &summary);
    if (rc != IFS_FS_OK) return rc;
    rc = describe(&summary, host->volume_bytes, &measured);
    if (rc != IFS_FS_OK) return rc;
    uint64_t verified_free = 0u;
    rc = ufs_verify_free_bitmap_host_v2(host, &summary, &verified_free);
    if (rc != IFS_FS_OK) return rc;
    if (verified_free > UINT64_MAX / summary.fragment_size ||
        verified_free * summary.fragment_size != measured.free_bytes)
        return IFS_FS_CORRUPT;
    slot->info = measured;
    slot->host = host;
    slot->summary = summary;
    slot->map_inventory = NULL;
    slot->active = ++session_sequence;
    *info = measured;
    *volume = (void *)(uintptr_t)slot->active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    if (slot->map_inventory)
        ufs_map_host_close_v2(slot->map_inventory);
    slot->map_inventory = NULL;
    slot->host = NULL;
    slot->summary = (LdUfsSummary){0};
    slot->active = 0u;
    slot->info = (IfsFsVolumeInfoV2){0};
    return IFS_FS_OK;
}
static int query_volume(void *volume, IfsFsVolumeInfoV2 *info)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *info = slot->info;
    return IFS_FS_OK;
}
static int ensure_inventory(Ufs2Session *slot)
{
    if (!slot->map_inventory)
        return ufs_map_host_open_v2(slot->host, &slot->summary,
                                    &slot->map_inventory);
    return IFS_FS_OK;
}
static int map_path(void *volume, const char *path, uint64_t first_extent,
                    IfsFsExtentV2 *extents, uint32_t capacity, uint32_t *written)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot || !written) return IFS_FS_INVALID;
    *written = 0u;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_map_host_page_v2(slot->map_inventory, path, first_extent,
                             extents, capacity, written) : rc;
}
static const IfsFsDefragOpsV2 defrag_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsDefragOpsV2),
               IFS_FS_GROUP_DEFRAG, 0u, IFS_FS_DEFRAG_MAP},
    .map = map_path
};
static int stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_stat_host_v2(slot->map_inventory, path, entry) : rc;
}
static int directory_entry(void *volume, const char *path, uint32_t index,
                           IfsFsEntryV2 *entry)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_list_host_v2(slot->map_inventory, path, index, entry) : rc;
}
static int read_file(void *volume, const char *path, uint64_t offset,
                     void *destination, uint32_t length)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_read_host_v2(slot->map_inventory, path, offset,
                         destination, length) : rc;
}
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE, 0u,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
               IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ},
    .probe = probe,
    .mount = mount_volume,
    .unmount = unmount_volume,
    .stat = stat_path,
    .directory_entry = directory_entry,
    .read = read_file
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
    if (group_id == IFS_FS_GROUP_DEFRAG) return &defrag_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "ufs2",
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG),
    .query_group = query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
