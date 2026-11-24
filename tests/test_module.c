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
static const IfsFsCoreOpsV2 *unmount_during_read;
static void *active_volume;
static int observed_unmount;
static int host_read(void *context,uint64_t offset,void *buffer,uint32_t bytes)
{
    Host *h=context;
    if (unmount_during_read) {
        const IfsFsCoreOpsV2 *ops=unmount_during_read;
        unmount_during_read=NULL;
        observed_unmount=ops->unmount(active_volume);
        if (observed_unmount!=IFS_FS_BUSY) return -1;
    }
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

static void put16(unsigned char *p,size_t at,uint16_t v,int little)
{
    for(unsigned i=0u;i<2u;++i)
        p[at+i]=(unsigned char)(v>>(8u*(little?i:1u-i)));
}
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
    unsigned char cg[512]={0};
    put32(cg,4u,0x00090255u,little);
    put32(cg,12u,0u,little);
    put32(cg,20u,64u,little);
    put32(cg,92u,168u,little);
    put32(cg,96u,176u,little);
    for(unsigned bit=0u;bit<42u;++bit)
        cg[176u+(bit>>3u)] |= (unsigned char)(1u << (bit & 7u));
    if(pwrite(fd,cg,sizeof cg,1024)!=(ssize_t)sizeof cg) return -1;
    return pwrite(fd,sb,sizeof sb,8192)==(ssize_t)sizeof sb ? 0:-1;
}
/* Real UFS2 256-byte inode namespace sharing the native V2 extent and
 * BSD dirent implementation. No kernel or host pathname reader is involved. */
static int write_file_fixture(int fd,int little)
{
    unsigned char cg[512]={0},root[256]={0},file[256]={0};
    unsigned char directory[512]={0},payload[1024]={0};
    put32(cg,4u,0x00090255u,little);
    put32(cg,12u,0u,little);
    put32(cg,20u,64u,little);
    put32(cg,92u,168u,little);
    put32(cg,96u,176u,little);
    cg[168u]=(unsigned char)((1u<<2u)|(1u<<3u));
    for(unsigned bit=22u;bit<64u;++bit)
        cg[176u+(bit>>3u)]|=(unsigned char)(1u<<(bit&7u));
    put16(root,0u,UINT16_C(0040755),little);
    put64(root,16u,512u,little);
    put64(root,112u,16u,little);
    put16(file,0u,UINT16_C(0100644),little);
    put64(file,16u,1024u,little);
    put64(file,112u,18u,little);
    put32(directory,0u,2u,little);
    put16(directory,4u,12u,little);
    directory[6u]=4u;directory[7u]=1u;
    memcpy(directory+8u,".",1u);
    put32(directory,12u,2u,little);
    put16(directory,16u,12u,little);
    directory[18u]=4u;directory[19u]=2u;
    memcpy(directory+20u,"..",2u);
    put32(directory,24u,3u,little);
    put16(directory,28u,12u,little);
    directory[30u]=8u;directory[31u]=4u;
    memcpy(directory+32u,"test",4u);
    put32(directory,36u,3u,little);
    put16(directory,40u,476u,little);
    directory[42u]=8u;directory[43u]=11u;
    memcpy(directory+44u,"example.txt",11u);
    memcpy(payload,"UFS2 native Common V2 inode data",32u);
    return pwrite(fd,cg,sizeof(cg),1024)==(ssize_t)sizeof(cg) &&
           pwrite(fd,root,sizeof(root),2048+2*256)==(ssize_t)sizeof(root) &&
           pwrite(fd,file,sizeof(file),2048+3*256)==(ssize_t)sizeof(file) &&
           pwrite(fd,directory,sizeof(directory),16*1024)==(ssize_t)sizeof(directory) &&
           pwrite(fd,payload,sizeof(payload),18*1024)==(ssize_t)sizeof(payload)
           ? 0:-1;
}
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ufs2"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    CHECK(api->group_mask==(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                           IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) |
                           IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)));
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *ops=(const IfsFsVolumeOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *defrag=(const IfsFsDefragOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(core && ops && defrag);
    CHECK(defrag->header.capabilities==IFS_FS_DEFRAG_MAP);
    CHECK(defrag->map && !defrag->plan && !defrag->execute &&
          !defrag->verify && !defrag->recover && !defrag->release_plan);
    CHECK(core->header.capabilities==(IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
           IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ));
    CHECK(ops->header.capabilities==(IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_FORMAT));
    CHECK(core->probe && core->mount && core->unmount && ops->query);
    CHECK(core->stat && core->directory_entry && core->read &&
          !core->write && !core->create && !core->remove);
    CHECK(ops->format && !ops->repair && !ops->sync);
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
    CHECK(core->mount(&host,&previous,&info)==IFS_FS_OK);
    CHECK(previous!=NULL && previous!=session);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(previous,&info)==IFS_FS_OK);
    CHECK(core->unmount(previous)==IFS_FS_OK);
    CHECK(ops->query(previous,&info)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(session,&info)==IFS_FS_OK);
    IfsFsExtentV2 extent={0};
    uint32_t count=123u;
    /* Synthetic geometry has no allocated root inode; never invent an
     * extent or treat magic-only media as mapped. */
    const int map_rc=defrag->map(session,"/",0u,&extent,1u,&count);
    if (map_rc != IFS_FS_NOT_FOUND)
        fprintf(stderr,"synthetic UFS2 MAP returned %d; expected unallocated root\n",map_rc);
    CHECK(map_rc==IFS_FS_NOT_FOUND);
    CHECK(count==0u);
    previous=session;
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->unmount(previous)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(previous,&info)==IFS_FS_INVALID);
    /* Fully populated UFS2 native filesystem fixtures qualify real reads. */
    CHECK(write_file_fixture(fd,1)==0);
    unsigned char before_reads[65536],after_reads[65536];
    CHECK(pread(fd,before_reads,sizeof(before_reads),0)==
          (ssize_t)sizeof(before_reads));
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_OK);
    void *parallel=NULL;
    CHECK(core->mount(&host,&parallel,&info)==IFS_FS_OK &&
          parallel!=NULL && parallel!=session);
    /* A firmware or hosted caller may reuse its stack descriptor after
     * mount. The reader must own a bounded V2 descriptor snapshot; only
     * the separately managed context and callbacks retain host lifetime. */
    {
        const IfsFsHostRuntimeV2 original = host;
        char data[32] = {0};
        host.read_at = host_read_failure;
        host.logical_sector_bytes = 4096u;
        /* A read through the retained host callback must not permit
         * reentrant unmount to tear down its lazy inventory. */
        active_volume=session;
        unmount_during_read=core;
        observed_unmount=IFS_FS_INVALID;
        CHECK(core->read(session,"/test",0u,data,32u)==IFS_FS_OK);
        CHECK(!unmount_during_read && observed_unmount==IFS_FS_BUSY);
        CHECK(memcmp(data,"UFS2 native Common V2 inode data",32u)==0);
        CHECK(core->read(parallel,"/test",0u,data,32u)==IFS_FS_OK);
        host = original;
    }
    {
        char name[256]={0},data[64]={0};
        IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                            .name_capacity=sizeof(name)};
        CHECK(core->stat(session,"/example.txt",&entry)==IFS_FS_OK);
        CHECK(entry.type==IFS_FS_ENTRY_FILE &&
              entry.size_bytes==1024u &&
              strcmp(name,"example.txt")==0);
        CHECK(core->directory_entry(session,"/",0u,&entry)==IFS_FS_OK &&
              strcmp(name,"test")==0);
        CHECK(core->directory_entry(session,"/",1u,&entry)==IFS_FS_OK &&
              strcmp(name,"example.txt")==0);
        CHECK(core->directory_entry(session,"/",2u,&entry)==IFS_FS_END);
        CHECK(core->read(session,"/test",0u,data,32u)==IFS_FS_OK);
        CHECK(memcmp(data,"UFS2 native Common V2 inode data",32u)==0);
        CHECK(core->read(session,"/test",1024u,data,1u)==IFS_FS_INVALID);
        /* The Common V2 allocation-map group must expose extents within
         * the authenticated partition and terminate pagination. */
        {
            IfsFsExtentV2 mapped={0};
            uint32_t written=0u;
            CHECK(defrag->map(session,"/test",0u,&mapped,1u,&written)==IFS_FS_OK);
            CHECK(written==1u && mapped.logical_offset==0u &&
                  mapped.length==1024u &&
                  mapped.physical_offset<=host.volume_bytes &&
                  mapped.length<=host.volume_bytes-mapped.physical_offset);
            CHECK(defrag->map(session,"/test",1u,&mapped,1u,&written)==IFS_FS_END);
            CHECK(written==0u);
        }
        CHECK(core->stat(session,"/missing",&entry)==IFS_FS_NOT_FOUND);
        CHECK(core->read(session,"/../test",0u,data,4u)==IFS_FS_INVALID);
        CHECK(core->read(session,"/",0u,data,4u)==IFS_FS_INVALID);
    }
    /* Keep both real Common V2 read-only mounts alive while a disposable
     * BSD directory acquires two individually valid entries named "test".
     * Reject the duplicate before returning a file inode or its bytes. */
    {
        unsigned char saved_dir[512],duplicate[512];
        char label[64]={0},data[4]={0};
        IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),
                            .name=label,.name_capacity=sizeof(label)};
        CHECK(pread(fd,saved_dir,sizeof(saved_dir),16u*1024u)
              ==(ssize_t)sizeof(saved_dir));
        memcpy(duplicate,saved_dir,sizeof(duplicate));
        put16(duplicate,40u,20u,1); /* example.txt, last, becomes 20B */
        put32(duplicate,56u,3u,1);
        put16(duplicate,60u,456u,1); /* duplicate consumes remaining bytes */
        duplicate[62u]=8u;duplicate[63u]=4u;
        memcpy(duplicate+64u,"test",4u);
        CHECK(pwrite(fd,duplicate,sizeof(duplicate),16u*1024u)
              ==(ssize_t)sizeof(duplicate));
        CHECK(core->stat(session,"/test",&entry)==IFS_FS_CORRUPT);
        CHECK(core->read(parallel,"/test",0u,data,1u)==IFS_FS_CORRUPT);
        CHECK(pwrite(fd,saved_dir,sizeof(saved_dir),16u*1024u)
              ==(ssize_t)sizeof(saved_dir));
        CHECK(core->stat(session,"/test",&entry)==IFS_FS_OK);
    }
    /* Unmounting one volume must not invalidate another module session.
     * Both readers must leave every byte of the source volume untouched. */
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(ops->query(session,&info)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(parallel,&info)==IFS_FS_OK);
    {
        char data[32]={0};
        CHECK(core->read(parallel,"/test",0u,data,32u)==IFS_FS_OK);
        CHECK(memcmp(data,"UFS2 native Common V2 inode data",32u)==0);
    }
    CHECK(core->unmount(parallel)==IFS_FS_OK);
    CHECK(ops->query(parallel,&info)==IFS_FS_INVALID);
    CHECK(pread(fd,after_reads,sizeof(after_reads),0)==
          (ssize_t)sizeof(after_reads));
    CHECK(memcmp(before_reads,after_reads,sizeof(before_reads))==0);
    /* A valid superblock with a corrupted cylinder-group free map must be
     * rejected rather than exposing unverified recorded free totals. */
    unsigned char damaged=0u;
    CHECK(pwrite(fd,&damaged,1u,1024+176+5)==1);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&session,&info)==IFS_FS_CORRUPT);
    CHECK(session==NULL);
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
