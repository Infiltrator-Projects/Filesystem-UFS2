/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/random.h>
typedef struct {int fd;uint32_t sector;uint64_t bytes;unsigned writes;} Media;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    return pread(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    ++m->writes;
    return pwrite(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int flush(void *ctx){return fsync(((Media*)ctx)->fd);}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static int entropy(void *ctx,void *p,uint32_t n)
{(void)ctx;return getrandom(p,n,0)==(ssize_t)n?0:-1;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"UFS2 format line %d: %s\n",__LINE__,#x);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=5)return 2;
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
    IfsFsModuleApiEntryV2 get=NULL;
    void *symbol=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);
    if(!symbol || sizeof(get)!=sizeof(symbol))return 2;
    memcpy(&get,&symbol,sizeof(get));
    const IfsFsModuleApiV2 *api=get();
    if(!ifs_fs_api_v2_valid(api,"ufs2"))return 2;
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!core || !vol || !vol->format)return 2;
    Media m={.sector=(uint32_t)strtoul(argv[4],NULL,10),.bytes=128ull*1024u*1024u};
    uint32_t variant=(uint32_t)strtoul(argv[3],NULL,10);
    m.fd=open(argv[2],O_CREAT|O_EXCL|O_NOFOLLOW|O_RDWR,0600);
    if(m.fd<0)return 2;
    int result=1;
    CHECK(ftruncate(m.fd,(off_t)m.bytes)==0);
    IfsFsHostRuntimeV2 h={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=m.bytes,.logical_sector_bytes=m.sector,
        .physical_sector_bytes=m.sector,.maximum_transfer_bytes=m.sector,
        .context=&m,.read_at=rd,.write_at=wr,.flush=flush,
        .allocate=allocate,.release=release,.random_bytes=entropy
    },ro=h;
    ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
    CHECK(vol->format(&ro,variant,"NativeUFS",0u)==IFS_FS_ACCESS);
    CHECK(vol->format(&h,0u,"NativeUFS",0u)==IFS_FS_UNSUPPORTED);
    CHECK(vol->format(&h,variant,"Invalid/Name",0u)==IFS_FS_INVALID);
    CHECK(!m.writes);
    CHECK(vol->format(&h,variant,"NativeUFS",0u)==IFS_FS_OK);
    uint32_t detected=0;void *mount=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    char name[128];
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,.name_capacity=sizeof(name)};
    CHECK(core->probe(&ro,&detected)==IFS_FS_OK && detected==variant);
    CHECK(core->mount(&ro,&mount,&info)==IFS_FS_OK && mount);
    CHECK(info.variant==variant && info.total_bytes==m.bytes &&
          info.free_bytes==m.bytes-46u*4096u);
    char label[33];
    uint32_t label_bytes=0;
    CHECK(vol->label && vol->label(mount,label,sizeof(label),&label_bytes)==IFS_FS_OK &&
          label_bytes==9u && !strcmp(label,"NativeUFS"));
    const IfsFsMetadataOpsV2 *meta=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_METADATA);
    IfsFsMetadataV2 metadata={.structure_bytes=sizeof(metadata)};
    CHECK(meta && meta->get && meta->get(mount,"/",&metadata)==IFS_FS_OK &&
          metadata.mode==0040755u && !metadata.owner && !metadata.group);
    CHECK(core->stat(mount,"/",&entry)==IFS_FS_OK && entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->directory_entry(mount,"/",0u,&entry)==IFS_FS_END);
    CHECK(vol->format(&h,variant,"NativeUFS",0u)==IFS_FS_BUSY);
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(core->mount(&h,&mount,&info)==IFS_FS_ACCESS && !mount);
    puts("UFS2 native format, allocation and empty namespace PASS");
    result=0;
done:
    close(m.fd);dlclose(lib);
    if(result)unlink(argv[2]);
    return result;
}
