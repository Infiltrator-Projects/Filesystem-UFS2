/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict UFS2 Common V2: endian probes, checked read-only volume
 * lifecycle, 4Kn host I/O, invalid geometry and stale-handle refusal.
 * Synthetic superblocks validate geometry plumbing, not file operations.
 */
#define _GNU_SOURCE
#include <infiltratr/filesystem_module_v2.h>
#include "ufs_native.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "UFS2 V2 failure at %d: %s\n",__LINE__,#x); \
    return 1; } } while (0)

typedef struct { int fd; uint32_t sector; } Host;
static int host_read(void *context,uint64_t offset,void *buffer,uint32_t bytes)
{
    Host *h=context;
    if (offset % h->sector || bytes % h->sector) return IFS_FS_INVALID;
    return pread(h->fd,buffer,bytes,(off_t)offset)==(ssize_t)bytes ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int host_read_failure(void *context,uint64_t offset,void *buffer,uint32_t bytes)
{
    (void)context;(void)offset;(void)buffer;(void)bytes;
    return IFS_FS_IO_ERROR;
}
static int host_write(void *context,uint64_t offset,const void *buffer,uint32_t bytes)
{
    (void)context;(void)offset;(void)buffer;(void)bytes;
    return IFS_FS_IO_ERROR;
}
static int host_flush(void *context) { (void)context;return IFS_FS_OK; }
static void *host_allocate(void *context,size_t bytes)
{ (void)context;return malloc(bytes); }
static void host_release(void *context,void *ptr)
{ (void)context;free(ptr); }

static void put32(unsigned char *p,size_t at,uint32_t v,int little)
{
    for (unsigned i=0u;i<4u;++i)
        p[at+i]=(unsigned char)(v >> (8u*(little?i:3u-i)));
}
static void put64(unsigned char *p,size_t at,uint64_t v,int little)
{
    for (unsigned i=0u;i<8u;++i)
        p[at+i]=(unsigned char)(v >> (8u*(little?i:7u-i)));
}
/* A bounded superblock with cross-field-consistent allocation counts.
 * This is not a formatted, traversable UFS namespace or recovery test. */
static int write_geometry(int fd,const unsigned char magic[4],int little,
                          int corrupt)
{
    unsigned char sb[8192]={0};
    put32(sb,8u,8u,little);      /* superblock fragment */
    put32(sb,12u,1u,little);     /* cylinder-group fragment */
    put32(sb,16u,2u,little);     /* inode fragment */
    put32(sb,20u,4u,little);     /* data fragment */
    put32(sb,44u,1u,little);     /* cylinder groups */
    put32(sb,48u,corrupt ? 1234u:4096u,little);
    put32(sb,52u,1024u,little);
    put32(sb,56u,4u,little);
    put32(sb,116u,512u,little);
    put32(sb,120u,16u,little);
    put32(sb,160u,512u,little); /* CG header bytes */
    put32(sb,184u,16u,little);  /* inodes per group */
    put32(sb,188u,64u,little);  /* fragments per group */
    sb[209]=1u;                 /* clean */
    put64(sb,1080u,64u,little);
    put64(sb,1088u,56u,little);
    put64(sb,1016u,10u,little);
    put64(sb,1032u,2u,little);
    memcpy(sb+1372u,magic,4u);
    return pwrite(fd,sb,sizeof sb,8192)==(ssize_t)sizeof sb ? 0:-1;
}
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ufs2"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    CHECK(api->group_mask==(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                           IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)));
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *ops=(const IfsFsVolumeOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && ops);
    CHECK(core->header.capabilities==(IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT));
    CHECK(ops->header.capabilities==IFS_FS_VOLUME_QUERY);
    CHECK(core->probe && core->mount && core->unmount && ops->query);
    CHECK(!core->read && !core->write && !core->create && !core->remove);
    CHECK(!ops->format && !ops->repair && !ops->sync);
    char path[]="/tmp/ufs2-v2-geometry-XXXXXX";
    int fd=mkstemp(path);
    CHECK(fd>=0);
    CHECK(ftruncate(fd,65536)==0);
    static const unsigned char foreign[4]={0x54,0x19,0x01,0x00};
    static const unsigned char le[4]={0x19,0x01,0x54,0x19};
    static const unsigned char be[4]={0x19,0x54,0x01,0x19};
    Host context={fd,512u};
    IfsFsHostRuntimeV2 host={
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=65536u,
        .logical_sector_bytes=512u,
        .physical_sector_bytes=512u,
        .maximum_transfer_bytes=4096u,
        .context=&context,
        .read_at=host_read,
        .allocate=host_allocate,
        .release=host_release
    };
    CHECK(ifs_fs_host_v2_valid(&host));
    uint32_t variant=0u;
    void *session=NULL,*previous=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
    /* Magic-only probe remains valid for discovery, never for mount. */
    CHECK(pwrite(fd,le,4u,8192+1372)==4);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_UNSUPPORTED);
    CHECK(session==NULL);
    CHECK(write_geometry(fd,le,1,0)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK &&
          variant==LD_UFS_VARIANT_UFS2_LE);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_OK);
    CHECK(session!=NULL);
    CHECK(info.variant==variant && info.block_bytes==4096u);
    CHECK(info.total_bytes==65536u && info.free_bytes==43008u);
    CHECK(info.max_name_bytes==255u);
    CHECK(core->mount(&host,&previous,&info)==IFS_FS_BUSY);
    CHECK(previous==NULL);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(session,&info)==IFS_FS_OK);
    previous=session;
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->unmount(previous)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(previous,&info)==IFS_FS_INVALID);
    CHECK(write_geometry(fd,be,0,0)==0);
    context.sector=4096u;
    host.logical_sector_bytes=4096u;
    host.physical_sector_bytes=4096u;
    host.maximum_transfer_bytes=4096u;
    CHECK(core->probe(&host,&variant)==IFS_FS_OK &&
          variant==LD_UFS_VARIANT_UFS2_BE);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_OK);
    CHECK(session!=previous);
    CHECK(ops->query(previous,&info)==IFS_FS_INVALID);
    CHECK(ops->query(session,&info)==IFS_FS_OK);
    CHECK(core->unmount(session)==IFS_FS_OK);
    /* Reject corrupted geometry, foreign UFS version and hostile I/O. */
    CHECK(write_geometry(fd,be,0,1)==0);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_UNSUPPORTED);
    CHECK(write_geometry(fd,foreign,1,0)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_NOT_FOUND);
    CHECK(write_geometry(fd,le,1,0)==0);
    IfsFsHostRuntimeV2 invalid=host;
    invalid.read_at=NULL;
    CHECK(core->probe(&invalid,&variant)==IFS_FS_INVALID);
    CHECK(core->mount(&invalid,&session,&info)==IFS_FS_INVALID);
    invalid=host;
    invalid.read_at=host_read_failure;
    CHECK(core->probe(&invalid,&variant)==IFS_FS_IO_ERROR);
    CHECK(core->mount(&invalid,&session,&info)==IFS_FS_IO_ERROR);
    invalid=host;
    invalid.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    invalid.write_at=host_write;
    invalid.flush=host_flush;
    CHECK(ifs_fs_host_v2_valid(&invalid));
    CHECK(core->mount(&invalid,&session,&info)==IFS_FS_ACCESS);
    CHECK(close(fd)==0);
    CHECK(unlink(path)==0);
    puts("UFS2-STRICT-COMMON-V2-READONLY-VOLUME-PASS");
    return 0;
}
