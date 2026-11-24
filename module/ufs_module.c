/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS2: strict Common V2 probe, read-only lifecycle, and volume query.
 * The independent UFS engine remains the only decoder for disk geometry.
 * No unqualified file operations, relocation, writes, or legacy ABI exports.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdatomic.h>
#include "ufs_native.h"
#ifdef IFS_UFS_FIRMWARE_BUILD
extern void ifs_ufs_firmware_bind_host(const IfsFsHostRuntimeV2 *);
#endif

__attribute__((used,retain,section(".ifsmeta")))
static const char identity[] = "IFS-MODULE:ufs2:common2";

/* The lazy inventory and freestanding allocator's host binding are shared
 * mutable state. Until they are fully per-session, a nonblocking operation
 * gate protects all callbacks, including reentrant calls from host I/O.
 * No spin-wait is permitted while an external callback is executing. */
static atomic_flag operation_gate=ATOMIC_FLAG_INIT;
static int enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&operation_gate,memory_order_acquire);
}
static int leave_operation(int rc)
{
    atomic_flag_clear_explicit(&operation_gate,memory_order_release);
    return rc;
}

static int inspect(const IfsFsHostRuntimeV2 *host, LdUfsSummary *summary)
{
    int rc = ufs_read_summary_host_v2(host, summary);
    if (rc != IFS_FS_OK) return rc;
    if (summary->variant != LD_UFS_VARIANT_UFS2_LE &&
        summary->variant != LD_UFS_VARIANT_UFS2_BE)
        return IFS_FS_NOT_FOUND;
    return IFS_FS_OK;
}

static int probe_impl(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    LdUfsSummary summary;
    int rc;
    if (!variant) return IFS_FS_INVALID;
    *variant = 0u;
    rc = inspect(host, &summary);
    if (rc == IFS_FS_OK) *variant = (uint32_t)summary.variant;
    return rc;
}
static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(probe_impl(host,variant));
}

/* This is a strictly read-only geometry session, not a writable filesystem
 * mount. The superblock must have validated allocation/cylinder geometry;
 * recognising a four-byte UFS magic alone is NOT enough for volume query.
 * Generation tokens reject stale callers without dereferencing freed memory.
 */
#define UFS2_MAX_SESSIONS 32u
/* The provider is shared across mounted volumes. A monotonically increasing
 * opaque token identifies a mount, never an address of a reusable slot.
 * Previously unmounted tokens cannot become valid again. The provider
 * serializes callbacks while its firmware runtime has a shared host binding. */
typedef struct {
    uintptr_t active;
    IfsFsVolumeInfoV2 info;
    IfsFsHostRuntimeV2 host;
    LdUfsSummary summary;
    void *map_inventory;
} Ufs2Session;
static Ufs2Session sessions[UFS2_MAX_SESSIONS];
static uintptr_t session_sequence;

/* Copy only fields present in the caller's V2 host prefix. A consumer may
 * allocate its descriptor on the stack and reuse it after mount; the
 * filesystem may retain callback context, but not descriptor storage. */
static void retain_host(IfsFsHostRuntimeV2 *dst,
                        const IfsFsHostRuntimeV2 *src)
{
    const size_t bytes = src->structure_bytes < sizeof(*dst) ?
        src->structure_bytes : sizeof(*dst);
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    for (size_t i = 0u; i < sizeof(*dst); ++i)
        out[i] = i < bytes ? in[i] : 0u;
    dst->structure_bytes = (uint32_t)bytes;
}

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

static int mount_volume_impl(const IfsFsHostRuntimeV2 *host, void **volume,
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
    retain_host(&slot->host, host);
    slot->summary = summary;
    slot->map_inventory = NULL;
    slot->active = ++session_sequence;
    *info = measured;
    *volume = (void *)(uintptr_t)slot->active;
    return IFS_FS_OK;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host, void **volume, IfsFsVolumeInfoV2 *info)
{
    if (volume) *volume=NULL;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(mount_volume_impl(host,volume,info));
}
static int unmount_volume_impl(void *volume)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    if (slot->map_inventory)
        ufs_map_host_close_v2(slot->map_inventory);
    slot->map_inventory = NULL;
    slot->host = (IfsFsHostRuntimeV2){0};
    slot->summary = (LdUfsSummary){0};
    slot->active = 0u;
    slot->info = (IfsFsVolumeInfoV2){0};
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(unmount_volume_impl(volume));
}
static int query_volume_impl(void *volume, IfsFsVolumeInfoV2 *info)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *info = slot->info;
    return IFS_FS_OK;
}
static int query_volume(void *volume, IfsFsVolumeInfoV2 *info)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(query_volume_impl(volume,info));
}
static int ensure_inventory(Ufs2Session *slot)
{
#ifdef IFS_UFS_FIRMWARE_BUILD
    ifs_ufs_firmware_bind_host(&slot->host);
#endif
    if (!slot->map_inventory)
        return ufs_map_host_open_v2(&slot->host, &slot->summary,
                                    &slot->map_inventory);
    return IFS_FS_OK;
}
static int map_path_impl(void *volume, const char *path, uint64_t first_extent,
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
static int map_path(void *volume, const char *path, uint64_t first_extent, IfsFsExtentV2 *extents, uint32_t capacity, uint32_t *written)
{
    if (written) *written=0u;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(map_path_impl(volume,path,first_extent,extents,capacity,written));
}
static const IfsFsDefragOpsV2 defrag_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsDefragOpsV2),
               IFS_FS_GROUP_DEFRAG, 0u, IFS_FS_DEFRAG_MAP},
    .map = map_path
};
static int stat_path_impl(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_stat_host_v2(slot->map_inventory, path, entry) : rc;
}
static int stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(stat_path_impl(volume,path,entry));
}
static int directory_entry_impl(void *volume, const char *path, uint32_t index,
                           IfsFsEntryV2 *entry)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_list_host_v2(slot->map_inventory, path, index, entry) : rc;
}
static int directory_entry(void *volume, const char *path, uint32_t index, IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(directory_entry_impl(volume,path,index,entry));
}
static int read_file_impl(void *volume, const char *path, uint64_t offset,
                     void *destination, uint32_t length)
{
    Ufs2Session *slot = find_session(volume);
    if (!slot) return IFS_FS_INVALID;
    int rc = ensure_inventory(slot);
    return rc == IFS_FS_OK ?
        ufs_read_host_v2(slot->map_inventory, path, offset,
                         destination, length) : rc;
}
static int read_file(void *volume, const char *path, uint64_t offset, void *destination, uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(read_file_impl(volume,path,offset,destination,length));
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
#ifndef IFS_UFS_FIRMWARE_BUILD
/* Native little-endian UFS2 format, using the FreeBSD FFS on-disk
 * structures documented in sys/ufs/ffs/fs.h and sys/ufs/ufs/dinode.h.
 * Initial qualification: one cylinder group, 16 KiB blocks, 4 KiB
 * fragments, 256 inodes, no soft updates, snapshots or metadata hashes.
 * All disk access is through the exclusive Common V2 host lease.
 * An independent fsck_ffs run gates release of the resulting media. */
enum { FMT_BSIZE=16384u,FMT_FSIZE=4096u,FMT_FRAG=4u,
       FMT_IPG=256u,FMT_INODE=256u,FMT_PRIMARY=65536u,
       FMT_SBLK=(FMT_PRIMARY+8192u+FMT_BSIZE-1u)/FMT_BSIZE*FMT_FRAG,
       FMT_CBLK=FMT_SBLK+FMT_FRAG,FMT_IBLK=FMT_CBLK+FMT_FRAG,
       FMT_DBLK=FMT_IBLK+(FMT_IPG*FMT_INODE)/FMT_FSIZE,
       FMT_IUSED=168u,FMT_FREE=FMT_IUSED+FMT_IPG/8u };
static void fmt16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void fmt32(uint8_t *p,uint32_t x)
{fmt16(p,x);fmt16(p+2,x>>16);}
static void fmt64(uint8_t *p,uint64_t x)
{fmt32(p,(uint32_t)x);fmt32(p+4,(uint32_t)(x>>32));}
static int fmt_write(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t bytes)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    return ifs_fs_host_write_bytes_v2(h,at,p,bytes);
}
static int format_impl(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                       const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(variant!=3u || flags || h->logical_sector_bytes>FMT_FSIZE ||
       h->volume_bytes%FMT_BSIZE ||
       h->volume_bytes<16ull*1024u*1024u ||
       h->volume_bytes>128ull*1024u*1024u)return IFS_FS_UNSUPPORTED;
    for(unsigned i=0;i<UFS2_MAX_SESSIONS;++i)
        if(sessions[i].active)return IFS_FS_BUSY;
    if(!label)label="";
    size_t label_bytes=0u;
    for(;label[label_bytes];++label_bytes) {
        const unsigned char c=(unsigned char)label[label_bytes];
        if(label_bytes>=31u || c<32u || c>126u || c=='/')return IFS_FS_INVALID;
    }
    const uint32_t total=(uint32_t)(h->volume_bytes/FMT_FSIZE);
    const uint32_t cgsize=(FMT_FREE+(total+7u)/8u+FMT_FSIZE-1u)/FMT_FSIZE*FMT_FSIZE;
    const uint32_t root=FMT_DBLK+1u; /* first fragment is cylinder summary */
    const uint32_t first_free=root+1u;
    const uint32_t partial=(FMT_FRAG-first_free%FMT_FRAG)%FMT_FRAG;
    const uint32_t nbfree=(total-first_free-partial)/FMT_FRAG;
    const uint32_t nifree=FMT_IPG-3u,dsize=total-FMT_DBLK-1u;
    uint32_t now=0u,id2=0u;
    if(h->realtime_ns) {
        uint64_t seconds=h->realtime_ns(h->context)/1000000000ull;
        if(seconds<=UINT32_MAX)now=(uint32_t)seconds;
    }
    if(h->structure_bytes>=sizeof(*h) && h->random_bytes) {
        if(ifs_fs_host_random_bytes_v2(h,&id2,sizeof(id2))!=IFS_FS_OK)
            return IFS_FS_IO_ERROR;
    }
    uint8_t *block=h->allocate(h->context,FMT_BSIZE);
    uint8_t *sb=h->allocate(h->context,8192u);
    if(!block || !sb) {
        if(block)h->release(h->context,block);
        if(sb)h->release(h->context,sb);
        return IFS_FS_NO_MEMORY;
    }
    for(uint32_t i=0;i<FMT_BSIZE;++i)block[i]=0u;
    for(uint32_t i=0;i<8192u;++i)sb[i]=0u;
    int rc=IFS_FS_OK;
    /* Revoke all standard candidate signatures and the intended backup
     * before writing any allocation metadata. Never touch other volumes. */
    static const uint64_t candidates[]={0u,8192u,65536u,262144u,
                                        (uint64_t)FMT_SBLK*FMT_FSIZE};
    for(unsigned i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i) {
        rc=fmt_write(h,candidates[i],block,8192u);
        if(rc!=IFS_FS_OK)goto done;
    }
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}

    /* Cylinder group counts, inode map, and fragment allocation map.
     * Bits are little-endian within each byte; one means free fragments,
     * while inode-map one means allocated. Inodes zero and one reserved. */
    fmt32(block+4,0x00090255u);
    fmt32(block+20,total);fmt32(block+24,1u);
    fmt32(block+28,nbfree);fmt32(block+32,nifree);
    fmt32(block+36,partial);
    if(partial)fmt32(block+52u+4u*partial,1u);
    fmt32(block+116,FMT_IPG);fmt32(block+120,FMT_IPG);fmt64(block+136,now);
    fmt32(block+92,FMT_IUSED);fmt32(block+96,FMT_FREE);
    fmt32(block+100,FMT_FREE+(total+7u)/8u);
    block[FMT_IUSED]=7u;
    for(uint32_t bit=first_free;bit<total;++bit)
        block[FMT_FREE+bit/8u]|=(uint8_t)(1u<<(bit%8u));
    rc=fmt_write(h,(uint64_t)FMT_CBLK*FMT_FSIZE,block,FMT_BSIZE);
    if(rc!=IFS_FS_OK)goto done;

    /* Zero every inode block, then create root inode two. */
    for(uint32_t i=0;i<FMT_IPG*FMT_INODE/FMT_BSIZE;++i) {
        for(uint32_t j=0;j<FMT_BSIZE;++j)block[j]=0u;
        if(!i) {
            uint8_t *ino=block+2u*FMT_INODE;
            fmt16(ino,0040755u);fmt16(ino+2,2u);
            fmt32(ino+12,FMT_BSIZE);fmt64(ino+16,512u);
            fmt64(ino+24,FMT_FSIZE/512u);
            fmt64(ino+32,now);fmt64(ino+40,now);fmt64(ino+48,now);
            fmt64(ino+56,now);fmt32(ino+80,id2);fmt64(ino+112,root);
        }
        rc=fmt_write(h,((uint64_t)FMT_IBLK+i*FMT_FRAG)*FMT_FSIZE,block,FMT_BSIZE);
        if(rc!=IFS_FS_OK)goto done;
    }
    for(uint32_t i=0;i<FMT_BSIZE;++i)block[i]=0u;
    fmt32(block,1u);fmt32(block+4,nbfree);
    fmt32(block+8,nifree);fmt32(block+12,partial);
    rc=fmt_write(h,(uint64_t)FMT_DBLK*FMT_FSIZE,block,FMT_FSIZE);
    if(rc!=IFS_FS_OK)goto done;
    for(uint32_t i=0;i<FMT_FSIZE;++i)block[i]=0u;
    fmt32(block,2u);fmt16(block+4,12u);block[6]=4u;block[7]=1u;block[8]='.';
    fmt32(block+12,2u);fmt16(block+16,500u);block[18]=4u;
    block[19]=2u;block[20]='.';block[21]='.';
    rc=fmt_write(h,(uint64_t)root*FMT_FSIZE,block,FMT_FSIZE);
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}

    /* Complete superblock geometry, masks, cylinder summary, and both
     * modern and legacy count fields. No optional feature is asserted. */
    fmt32(sb+8,FMT_SBLK);fmt32(sb+12,FMT_CBLK);
    fmt32(sb+16,FMT_IBLK);fmt32(sb+20,FMT_DBLK);
    fmt32(sb+28,UINT32_MAX);fmt32(sb+32,now);
    fmt32(sb+36,total);fmt32(sb+40,dsize);fmt32(sb+44,1u);
    fmt32(sb+48,FMT_BSIZE);fmt32(sb+52,FMT_FSIZE);fmt32(sb+56,FMT_FRAG);
    fmt32(sb+60,8u);fmt32(sb+68,60u);
    fmt32(sb+72,~(FMT_BSIZE-1u));fmt32(sb+76,~(FMT_FSIZE-1u));
    fmt32(sb+80,14u);fmt32(sb+84,12u);fmt32(sb+88,1u);
    fmt32(sb+92,FMT_BSIZE/4u);fmt32(sb+96,2u);
    fmt32(sb+100,3u);fmt32(sb+104,FMT_FSIZE);
    const uint32_t nindir=FMT_BSIZE/8u;
    fmt32(sb+116,nindir);fmt32(sb+120,FMT_BSIZE/FMT_INODE);
    fmt32(sb+124,FMT_FSIZE/512u);fmt32(sb+136,1u);
    fmt32(sb+144,now);fmt32(sb+148,id2);
    fmt32(sb+152,FMT_DBLK);fmt32(sb+156,FMT_FSIZE);fmt32(sb+160,cgsize);
    fmt32(sb+168,total*8u);fmt32(sb+172,total*8u);
    fmt32(sb+176,1u);fmt32(sb+180,1u);
    fmt32(sb+184,FMT_IPG);fmt32(sb+188,total);
    fmt32(sb+192,1u);fmt32(sb+196,nbfree);
    fmt32(sb+200,nifree);fmt32(sb+204,partial);
    sb[209]=1u;sb[211]=0x80u;
    for(size_t i=0;i<label_bytes;++i)sb[680u+i]=(uint8_t)label[i];
    fmt32(sb+860,FMT_BSIZE);fmt64(sb+872,total);
    fmt64(sb+1000,FMT_PRIMARY);
    fmt64(sb+1008,1u);fmt64(sb+1016,nbfree);
    fmt64(sb+1024,nifree);fmt64(sb+1032,partial);
    fmt64(sb+1072,now);fmt64(sb+1080,total);fmt64(sb+1088,dsize);
    fmt64(sb+1096,FMT_DBLK);
    fmt32(sb+1196,16384u);fmt32(sb+1200,64u);
    fmt32(sb+1320,120u);fmt32(sb+1324,2u);
    const uint64_t maxfilesize=(uint64_t)FMT_BSIZE*
        (12ull+nindir+(uint64_t)nindir*nindir+(uint64_t)nindir*nindir*nindir)-1u;
    fmt64(sb+1328,maxfilesize);fmt64(sb+1336,FMT_BSIZE-1u);
    fmt64(sb+1344,FMT_FSIZE-1u);fmt32(sb+1352,0x7c269d38u-now);
    fmt32(sb+1356,1u);fmt32(sb+1360,1u);fmt32(sb+1372,0x19540119u);
    /* Commit redundant copy before the primary. The actual-copy
     * location field identifies each superblock independently. */
    fmt64(sb+992,(uint64_t)FMT_SBLK*FMT_FSIZE);
    rc=fmt_write(h,(uint64_t)FMT_SBLK*FMT_FSIZE,sb,8192u);
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    fmt64(sb+992,FMT_PRIMARY);
    rc=fmt_write(h,FMT_PRIMARY,sb,8192u);
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0) {
        for(uint32_t i=0;i<8192u;++i)sb[i]=0u;
        (void)ifs_fs_host_write_bytes_v2(h,FMT_PRIMARY,sb,8192u);
        (void)ifs_fs_host_write_bytes_v2(h,(uint64_t)FMT_SBLK*FMT_FSIZE,sb,8192u);
        (void)h->flush(h->context);rc=IFS_FS_IO_ERROR;goto done;
    }
    /* Qualify our result using the production geometry and bitmap
     * readers before allowing the caller to treat format as successful. */
    LdUfsSummary summary;
    rc=inspect(h,&summary);
    if(rc==IFS_FS_OK) {
        uint64_t free_fragments=0u;
        rc=ufs_verify_free_bitmap_host_v2(h,&summary,&free_fragments);
        if(rc==IFS_FS_OK && (summary.variant!=variant ||
           free_fragments!=total-first_free))rc=IFS_FS_CORRUPT;
    }
done:
    h->release(h->context,sb);h->release(h->context,block);
    return rc;
}
static int format_volume(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                         const char *label,uint32_t flags)
{
    if(!enter_operation())return IFS_FS_BUSY;
    return leave_operation(format_impl(h,variant,label,flags));
}
#endif /* hosted formatting */

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY
#ifndef IFS_UFS_FIRMWARE_BUILD
               | IFS_FS_VOLUME_FORMAT
#endif
    },
    .query = query_volume
#ifndef IFS_UFS_FIRMWARE_BUILD
    , .format = format_volume
#endif
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
    #ifdef IFS_UFS_FIRMWARE_BUILD
    .runtime_profiles = IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE,
#else
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
#endif
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
