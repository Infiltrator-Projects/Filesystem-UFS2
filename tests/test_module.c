/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS1 exclusion, UFS2 endian variants and hostile host I/O through V2.
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
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"UFS2 V2 failure: %s\n",#x);return 1;} } while(0)
static int host_read_failure(void *context,uint64_t offset,void *buffer,uint32_t bytes)
{
    (void)context;(void)offset;(void)buffer;(void)bytes;
    return IFS_FS_IO_ERROR;
}
static int host_read(void *context,uint64_t offset,void *buffer,uint32_t bytes)
{
    const int fd=*(const int *)context;
    return pread(fd,buffer,bytes,(off_t)offset)==(ssize_t)bytes ?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
static void *host_allocate(void *context,size_t bytes)
{ (void)context;return malloc(bytes); }
static void host_release(void *context,void *ptr)
{ (void)context;free(ptr); }

int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ufs2"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    CHECK(api->group_mask==IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE));
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->header.capabilities==IFS_FS_CORE_PROBE);
    CHECK(core->probe && !core->mount && !core->read && !core->write);
    char path[]="/tmp/ufs2-v2-only-test-XXXXXX";
    int fd=mkstemp(path);
    CHECK(fd>=0);
    static const unsigned char ufs1[4]={0x54,0x19,0x01,0x00};
    static const unsigned char ufs2_le[4]={0x19,0x01,0x54,0x19};
    static const unsigned char ufs2_be[4]={0x19,0x54,0x01,0x19};
    const off_t magic=8192+1372;
    CHECK(ftruncate(fd,65536)==0);
    IfsFsHostRuntimeV2 host={
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=65536,
        .logical_sector_bytes=512,
        .physical_sector_bytes=512,
        .maximum_transfer_bytes=4096,
        .context=&fd,
        .read_at=host_read,
        .allocate=host_allocate,
        .release=host_release
    };
    CHECK(ifs_fs_host_v2_valid(&host));
    uint32_t variant=0u;
    CHECK(pwrite(fd,ufs2_le,sizeof(ufs2_le),magic)==(ssize_t)sizeof(ufs2_le));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK &&
          variant==LD_UFS_VARIANT_UFS2_LE);
    CHECK(pwrite(fd,ufs2_be,sizeof(ufs2_be),magic)==(ssize_t)sizeof(ufs2_be));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK &&
          variant==LD_UFS_VARIANT_UFS2_BE);
    CHECK(pwrite(fd,ufs1,sizeof(ufs1),magic)==(ssize_t)sizeof(ufs1));
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND);
    IfsFsHostRuntimeV2 invalid=host;
    invalid.read_at=NULL;
    CHECK(core->probe(&invalid,&variant)==IFS_FS_INVALID);
    invalid=host;
    invalid.read_at=host_read_failure;
    CHECK(core->probe(&invalid,&variant)==IFS_FS_IO_ERROR);
    CHECK(close(fd)==0);
    CHECK(unlink(path)==0);
    puts("UFS2-STRICT-COMMON-V2-PROBE-PASS");
    return 0;
}
