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
#include <sys/wait.h>
typedef struct {int fd;uint32_t sector;uint64_t bytes;unsigned writes,flushes,stop_after;} Media;
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
static int flush(void *ctx)
{
    Media *m=ctx;
    int rc=fsync(m->fd);
    if(!rc && m->stop_after && ++m->flushes==m->stop_after)_exit(86);
    return rc;
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static int entropy(void *ctx,void *p,uint32_t n)
{(void)ctx;return getrandom(p,n,0)==(ssize_t)n?0:-1;}

/* Each boundary starts from an independent clone of the same fragmented
 * native volume. Skip only all-zero chunks while copying a sparse image;
 * the file content and geometry are exact. Crash from the actual host
 * flush callback, not a simulated successful return. */
static int clone_image(const Media *source,Media *dest)
{
    char path[]="/tmp/ufs2-redo-XXXXXX";
    *dest=*source;dest->fd=mkstemp(path);dest->writes=dest->flushes=dest->stop_after=0;
    if(dest->fd<0)return -1;
    unlink(path);
    if(ftruncate(dest->fd,(off_t)dest->bytes))goto fail;
    unsigned char *data=malloc(1024u*1024u);
    if(!data)goto fail;
    for(uint64_t at=0u;at<source->bytes;) {
        size_t n=(source->bytes-at>1024u*1024u)?1024u*1024u:(size_t)(source->bytes-at);
        if(pread(source->fd,data,n,(off_t)at)!=(ssize_t)n){free(data);goto fail;}
        size_t i=0u;while(i<n && !data[i])++i;
        if(i<n && pwrite(dest->fd,data,n,(off_t)at)!=(ssize_t)n){free(data);goto fail;}
        at+=n;
    }
    free(data);
    if(fsync(dest->fd))goto fail;
    return 0;
fail:
    close(dest->fd);dest->fd=-1;return -1;
}
static int check_payload(const IfsFsCoreOpsV2 *core,void *mount,
                         uint32_t block,const unsigned char *data)
{
    unsigned char actual[65536];
    for(uint32_t i=0;i<14u;++i)
        if(core->read(mount,"/alpha",(uint64_t)i*block,actual,block)!=IFS_FS_OK ||
           memcmp(actual,data,block))return -1;
    if(core->read(mount,"/tiny",0u,actual,block/2u+1u)!=IFS_FS_OK ||
       memcmp(actual,data,block/2u+1u))return -1;
    if(core->read(mount,"/beta",0u,actual,block)!=IFS_FS_OK ||
       memcmp(actual,data,block))return -1;
    if(core->read(mount,"/edge",0u,actual,1u)!=IFS_FS_OK || actual[0]!=data[0])return -1;
    return 0;
}
static int crash_recovery(const Media *source,const IfsFsHostRuntimeV2 *runtime,
                          const IfsFsCoreOpsV2 *core,const IfsFsDefragOpsV2 *map,
                          uint32_t block,const unsigned char *data)
{
    for(uint32_t policy=0u;policy<=10u;policy+=10u) {
        for(unsigned boundary=1u;boundary<=8u;++boundary) {
            Media scratch;
            if(clone_image(source,&scratch))return -1;
            IfsFsHostRuntimeV2 h=*runtime;h.context=&scratch;
            pid_t child=fork();
            if(child==0) {
                IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
                void *mount=NULL,*plan=NULL;uint64_t moved=0u;
                if(core->mount(&h,&mount,&info)!=IFS_FS_OK ||
                   map->plan(mount,"/",policy,&plan)!=IFS_FS_OK)_exit(87);
                scratch.stop_after=boundary;scratch.flushes=0u;
                (void)map->execute(mount,plan,&moved);_exit(87);
            }
            int status=0;
            if(child<0 || waitpid(child,&status,0)!=child ||
               !WIFEXITED(status) || WEXITSTATUS(status)!=86) {
                fprintf(stderr,"UFS2 crash boundary not reached: policy %u barrier %u\n",policy,boundary);
                close(scratch.fd);return -1;
            }
            void *mount=NULL;uint64_t repaired=0u,again=99u;
            IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
            IfsFsHostRuntimeV2 ro=h;ro.capabilities=IFS_FS_HOST_READ_ONLY;
            ro.write_at=NULL;ro.flush=NULL;
            /* After durable locator publication and before clearing it,
             * readers must refuse potentially half-published metadata. */
            if(boundary>=3u && boundary<=7u) {
                if(core->mount(&ro,&mount,&info)!=IFS_FS_BUSY || mount) {
                    close(scratch.fd);return -1;
                }
            }
            if(policy==0u && boundary==3u) {
                /* Corrupt the durable journal, not a live file. A bad
                 * digest must stop the cold mount without replay writes. */
                unsigned char marker[80],saved,damaged;
                if(pread(scratch.fd,marker,sizeof(marker),0)!=(ssize_t)sizeof(marker)) {
                    close(scratch.fd);return -1;
                }
                uint32_t first=(uint32_t)marker[8]|((uint32_t)marker[9]<<8)|
                    ((uint32_t)marker[10]<<16)|((uint32_t)marker[11]<<24);
                uint64_t at=(uint64_t)first*4096u+17u;
                if(!first || at>=scratch.bytes || pread(scratch.fd,&saved,1u,(off_t)at)!=1) {
                    close(scratch.fd);return -1;
                }
                damaged=(unsigned char)(saved^0x80u);
                unsigned before=scratch.writes;
                if(pwrite(scratch.fd,&damaged,1u,(off_t)at)!=1 || fsync(scratch.fd) ||
                   core->mount(&h,&mount,&info)!=IFS_FS_CORRUPT || mount ||
                   scratch.writes!=before ||
                   pwrite(scratch.fd,&saved,1u,(off_t)at)!=1 || fsync(scratch.fd)) {
                    close(scratch.fd);return -1;
                }
            }
            if(core->mount(&h,&mount,&info)!=IFS_FS_OK || !mount ||
               map->recover(mount,0u,&repaired)!=IFS_FS_OK ||
               repaired!=(boundary>=3u && boundary<=7u?1u:0u) ||
               map->recover(mount,0u,&again)!=IFS_FS_OK || again ||
               check_payload(core,mount,block,data) ||
               core->unmount(mount)!=IFS_FS_OK) {
                fprintf(stderr,"UFS2 recovery failed: policy %u barrier %u\n",policy,boundary);
                close(scratch.fd);return -1;
            }
            mount=NULL;
            if(core->mount(&ro,&mount,&info)!=IFS_FS_OK || !mount ||
               check_payload(core,mount,block,data) ||
               core->unmount(mount)!=IFS_FS_OK) {
                close(scratch.fd);return -1;
            }
            printf("UFS2 cold recovery policy %u barrier %u repaired %llu PASS\n",
                   policy,boundary,(unsigned long long)repaired);
            close(scratch.fd);
        }
    }
    return 0;
}

/* Verify the free space independently of the provider's private plan. */
static int check_growth_gap(const IfsFsDefragOpsV2 *map,
    const IfsFsAllocationOpsV2 *allocation,void *mount,const char *path,
    uint64_t size,uint64_t block,uint64_t capacity,uint64_t *gap_start,
    uint64_t *gap_end)
{
    uint64_t logical=0u,first=0u,last=0u,blocks=0u;
    for(;;) {
        IfsFsExtentV2 ext[16]={{0}};uint32_t count=0u;
        int rc=map->map(mount,path,first,ext,16u,&count);
        if(rc==IFS_FS_END) {
            if(count || logical!=size) return -1;
            break;
        }
        if(rc!=IFS_FS_OK || !count || count>16u)return -1;
        for(uint32_t i=0;i<count;++i) {
            const IfsFsExtentV2 *e=&ext[i];
            if(!e->length || e->logical_offset!=logical ||
               e->length>size-logical || e->physical_offset>capacity ||
               e->length>capacity-e->physical_offset ||
               (logical && e->physical_offset!=last))return -1;
            blocks+=e->length/block+(e->length%block!=0u);
            logical+=e->length;last=e->physical_offset+e->length;
        }
        first+=count;
    }
    uint64_t required=(blocks/10u+(blocks%10u!=0u))*block;
    uint64_t at=last;
    for(unsigned i=0u;i<2u;++i) {
        IfsFsAllocationRangeV2 range={0};uint32_t count=0u;
        uint64_t next=at,unknown=0u;
        int rc=allocation->map(mount,at,&range,1u,&count);
        if(rc!=IFS_FS_OK || count!=1u ||
           !ifs_fs_allocation_page_v2_valid(capacity,at,rc,&range,1u,count,&next,&unknown) ||
           unknown)return -1;
        if(range.state==IFS_FS_SPACE_FREE) {
            if(at-last>=block || range.length<required)return -1;
            *gap_start=at;*gap_end=at+required;return 0;
        }
        if(range.state!=IFS_FS_SPACE_ALLOCATED || next-last>=block)return -1;
        at=next;
    }
    return -1;
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"UFS2 format line %d: %s\n",__LINE__,#x);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=5 && argc!=6)return 2;
    if(argc==6 && strcmp(argv[5],"2048"))return 2;
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
    const int large=argc==6;
    Media m={.sector=(uint32_t)strtoul(argv[4],NULL,10),
             .bytes=(large?2048ull:128ull)*1024u*1024u};
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
          info.block_bytes==(large?65536u:16384u) &&
          info.free_bytes==m.bytes-(large?42ull*8192u:46ull*4096u));
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
    /* Exercise genuine data and indirect block allocation, partial-fragment
     * relocation, overwrite, a separate reopen, and complete native maps.
     * The independent FreeBSD checker subsequently inspects this populated
     * image, not an empty formatter-only fixture. */
    CHECK(core->create && core->write);
    CHECK(core->mount(&h,&mount,&info)==IFS_FS_OK && mount);
    uint32_t block=info.block_bytes;
    unsigned char data[65536],actual[65536];
    for(uint32_t i=0;i<block;++i)data[i]=(unsigned char)(i*17u+3u);
    CHECK(core->create(mount,"/alpha",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(mount,"/beta",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(mount,"/alpha",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS);
    CHECK(core->create(mount,"/directory",IFS_FS_ENTRY_DIRECTORY)==IFS_FS_UNSUPPORTED);
    CHECK(core->write(mount,"/alpha",0u,data,block)==IFS_FS_OK);
    CHECK(core->write(mount,"/beta",0u,data,block)==IFS_FS_OK);
    for(uint32_t i=1;i<14u;++i)
        CHECK(core->write(mount,"/alpha",(uint64_t)i*block,data,block)==IFS_FS_OK);
    CHECK(core->create(mount,"/tiny",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(mount,"/edge",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->write(mount,"/tiny",0u,data,1u)==IFS_FS_OK);
    CHECK(core->write(mount,"/edge",0u,data,1u)==IFS_FS_OK);
    CHECK(core->write(mount,"/tiny",1u,data+1,block/2u)==IFS_FS_OK);
    CHECK(core->write(mount,"/alpha",1u,data+1,511u)==IFS_FS_OK);
    CHECK(core->write(mount,"/alpha",(uint64_t)15u*block,data,1u)==IFS_FS_UNSUPPORTED);
    CHECK(core->stat(mount,"/alpha",&entry)==IFS_FS_OK && entry.size_bytes==(uint64_t)14u*block);
    const IfsFsDefragOpsV2 *planner=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(planner && planner->plan && planner->release_plan);
    unsigned before_plan=m.writes;
    void *plan=NULL,*other=NULL;
    CHECK(planner->plan(mount,"/missing",0u,&plan)==IFS_FS_NOT_FOUND && !plan);
    CHECK(planner->plan(mount,"/",1u,&plan)==IFS_FS_INVALID && !plan);
    CHECK(planner->plan(mount,"/",0u,&plan)==IFS_FS_OK && plan);
    CHECK(planner->plan(mount,"/",10u,&other)==IFS_FS_BUSY && !other);
    CHECK(core->write(mount,"/alpha",0u,data,1u)==IFS_FS_BUSY);
    CHECK(core->create(mount,"/blocked",IFS_FS_ENTRY_FILE)==IFS_FS_BUSY);
    planner->release_plan(mount,plan);
    CHECK(planner->plan(mount,"/alpha",10u,&other)==IFS_FS_OK && other && other!=plan);
    planner->release_plan(mount,plan);
    CHECK(planner->plan(mount,"/",0u,&plan)==IFS_FS_BUSY && !plan);
    planner->release_plan(mount,other);
    CHECK(planner->plan(mount,"/",10u,&plan)==IFS_FS_OK && plan);
    CHECK(m.writes==before_plan);
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(core->mount(&ro,&mount,&info)==IFS_FS_OK && mount);
    CHECK(planner->plan(mount,"/",0u,&other)==IFS_FS_ACCESS && !other);
    CHECK(core->write(mount,"/alpha",0u,data,1u)==IFS_FS_ACCESS);
    CHECK(core->create(mount,"/no",IFS_FS_ENTRY_FILE)==IFS_FS_ACCESS);
    for(uint32_t i=0;i<14u;++i) {
        CHECK(core->read(mount,"/alpha",(uint64_t)i*block,actual,block)==IFS_FS_OK);
        CHECK(!memcmp(actual,data,block));
    }
    CHECK(core->read(mount,"/tiny",0u,actual,block/2u+1u)==IFS_FS_OK);
    CHECK(!memcmp(actual,data,block/2u+1u));
    const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(map && map->map);
    const IfsFsAllocationOpsV2 *allocation=(const void*)
        ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    CHECK(allocation && allocation->map);
    uint64_t at=0u,unknown=0u,free_bytes=0u,used_bytes=0u;
    for(;;) {
        IfsFsAllocationRangeV2 ranges[3]={{0}};
        uint64_t next=at;
        uint32_t count=99u;
        int rc=allocation->map(mount,at,ranges,3u,&count);
        CHECK(ifs_fs_allocation_page_v2_valid(m.bytes,at,rc,ranges,3u,count,&next,&unknown));
        if(rc==IFS_FS_END)break;
        for(uint32_t i=0u;i<count;++i) {
            if(ranges[i].state==IFS_FS_SPACE_FREE)free_bytes+=ranges[i].length;
            else if(ranges[i].state==IFS_FS_SPACE_ALLOCATED ||
                    ranges[i].state==IFS_FS_SPACE_RESERVED)used_bytes+=ranges[i].length;
        }
        CHECK(next>at);
        at=next;
    }
    CHECK(at==m.bytes && !unknown && free_bytes==info.free_bytes &&
          used_bytes && free_bytes+used_bytes==m.bytes);
    uint64_t first=0,logical=0,prev=0;int fragmented=0;
    for(;;) {
        IfsFsExtentV2 ext[16]={{0}};uint32_t count=0;
        int rc=map->map(mount,"/alpha",first,ext,16u,&count);
        if(rc==IFS_FS_END){CHECK(!count);break;}
        CHECK(rc==IFS_FS_OK && count && count<=16u);
        for(uint32_t i=0;i<count;++i) {
            CHECK(ext[i].logical_offset==logical && ext[i].length);
            if(logical && ext[i].physical_offset!=prev)fragmented=1;
            logical+=ext[i].length;prev=ext[i].physical_offset+ext[i].length;
        }
        first+=count;CHECK(first<=14u);
    }
    CHECK(fragmented && logical==(uint64_t)14u*block);
    CHECK(core->unmount(mount)==IFS_FS_OK);
    if(!large && m.sector==512u)CHECK(crash_recovery(&m,&h,core,map,block,data)==0);
    CHECK(core->mount(&h,&mount,&info)==IFS_FS_OK && mount);
    uint64_t repaired=99u,moved=0u;
    CHECK(map->recover(mount,0u,&repaired)==IFS_FS_OK && !repaired);
    CHECK(map->plan(mount,"/",0u,&plan)==IFS_FS_OK && plan);
    CHECK(map->execute(mount,plan,&moved)==IFS_FS_OK && moved>0u);
    CHECK(map->verify(mount,plan)==IFS_FS_OK);
    map->release_plan(mount,plan);plan=NULL;
    CHECK(map->plan(mount,"/",10u,&plan)==IFS_FS_OK && plan);
    CHECK(map->execute(mount,plan,&moved)==IFS_FS_OK);
    CHECK(map->verify(mount,plan)==IFS_FS_OK);
    map->release_plan(mount,plan);plan=NULL;
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(core->mount(&ro,&mount,&info)==IFS_FS_OK && mount);
    for(uint32_t i=0;i<14u;++i) {
        CHECK(core->read(mount,"/alpha",(uint64_t)i*block,actual,block)==IFS_FS_OK);
        CHECK(!memcmp(actual,data,block));
    }
    CHECK(core->read(mount,"/tiny",0u,actual,block/2u+1u)==IFS_FS_OK);
    CHECK(!memcmp(actual,data,block/2u+1u));
    CHECK(core->read(mount,"/beta",0u,actual,block)==IFS_FS_OK && !memcmp(actual,data,block));
    CHECK(core->read(mount,"/edge",0u,actual,1u)==IFS_FS_OK && actual[0]==data[0]);
    first=0u;logical=0u;prev=0u;
    for(;;) {
        IfsFsExtentV2 ext[16]={{0}};uint32_t count=0u;
        int rc=map->map(mount,"/alpha",first,ext,16u,&count);
        if(rc==IFS_FS_END){CHECK(!count);break;}
        CHECK(rc==IFS_FS_OK && count);
        for(uint32_t i=0;i<count;++i) {
            CHECK(ext[i].logical_offset==logical && ext[i].length);
            CHECK(!logical || ext[i].physical_offset==prev);
            logical+=ext[i].length;prev=ext[i].physical_offset+ext[i].length;
        }
        first+=count;
    }
    CHECK(logical==(uint64_t)14u*block);
    const char *paths[]={"/alpha","/tiny","/beta","/edge"};
    uint64_t sizes[]={(uint64_t)14u*block,block/2u+1u,block,1u};
    uint64_t starts[4]={0},ends[4]={0};
    for(unsigned i=0u;i<4u;++i) {
        CHECK(check_growth_gap(map,allocation,mount,paths[i],sizes[i],
                               block,m.bytes,&starts[i],&ends[i])==0);
        for(unsigned j=0u;j<i;++j)
            CHECK(ends[i]<=starts[j] || ends[j]<=starts[i]);
    }
    CHECK(core->unmount(mount)==IFS_FS_OK);
    puts("UFS2 native format, create, write, fragmentation and independent reopen PASS");
    result=0;
done:
    close(m.fd);dlclose(lib);
    if(result)unlink(argv[2]);
    return result;
}
