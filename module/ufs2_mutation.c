/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hosted UFS2 mutation for the native clean, single-cylinder-group format.
 * This is a real FFS inode/directory/fragment allocator, not a media fixture.
 * Unsupported layouts, names, inode types, indirect levels and sparse growth
 * are rejected. Only the filesystem provider interprets these disk records.
 * The primary and backup superblocks are marked dirty and durably flushed
 * before mutation; failed I/O leaves the mount faulted and requires fsck.
 */
#include "ufs2_mutation.h"
#include <string.h>
#include <stdint.h>

enum { SB_BYTES=8192, SB_PRIMARY=65536, INODE_BYTES=256, INODES=256,
       CG_IUSED=168, CG_FREE=200, DIR_BYTES=512, MAX_BLOCK=65536,
       UFS2_MAGIC=0x19540119, CG_MAGIC=0x090255 };
typedef struct { uint32_t blocks,frags,inodes,runs[8]; } Counts;
struct Ufs2Mutation {
    const IfsFsHostRuntimeV2 *host;
    uint32_t bsize,fsize,frag,total,cgsize,sblk,cblk,iblk,dblk,root;
    uint8_t sb[SB_BYTES],cg[MAX_BLOCK],inodes[INODES*INODE_BYTES],dir[DIR_BYTES];
    Counts counts;
    int faulted;
};
typedef struct {uint32_t old,oldn,at,n;} BlockPlan;

static uint16_t get16(const uint8_t *p)
{return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t get32(const uint8_t *p)
{return get16(p)|((uint32_t)get16(p+2)<<16);}
static uint64_t get64(const uint8_t *p)
{return get32(p)|((uint64_t)get32(p+4)<<32);}
static void put16(uint8_t *p,uint32_t n)
{p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n)
{put16(p,n);put16(p+2,n>>16);}
static void put64(uint8_t *p,uint64_t n)
{put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static unsigned bit(const uint8_t *map,uint32_t n)
{return (map[n/8u]>>(n%8u))&1u;}
static void setbit(uint8_t *map,uint32_t n,unsigned value)
{
    uint8_t mask=(uint8_t)(1u<<(n%8u));
    if(value)map[n/8u]|=mask;else map[n/8u]&=(uint8_t)~mask;
}
static uint64_t disk(const Ufs2Mutation *w,uint32_t fragment)
{return (uint64_t)fragment*w->fsize;}
static uint64_t inode_disk(const Ufs2Mutation *w,uint32_t n)
{return disk(w,w->iblk)+(uint64_t)n*INODE_BYTES;}
static int rd(const Ufs2Mutation *w,uint64_t at,void *p,uint32_t n)
{return ifs_fs_host_read_bytes_v2(w->host,at,p,n);}
static int wr(const Ufs2Mutation *w,uint64_t at,const void *p,uint32_t n)
{return ifs_fs_host_write_bytes_v2(w->host,at,p,n);}
static int barrier(const Ufs2Mutation *w)
{return w->host->flush(w->host->context)==0?IFS_FS_OK:IFS_FS_IO_ERROR;}
static int cancelled(const Ufs2Mutation *w)
{return w->host->cancelled && w->host->cancelled(w->host->context);}
static uint64_t now(const Ufs2Mutation *w)
{return w->host->realtime_ns?w->host->realtime_ns(w->host->context)/1000000000ull:0u;}
static uint32_t blocks_for(const Ufs2Mutation *w,uint64_t bytes)
{return (uint32_t)((bytes+w->bsize-1u)/w->bsize);}
static uint32_t frags_for(const Ufs2Mutation *w,uint64_t bytes,uint32_t lbn)
{
    uint64_t at=(uint64_t)lbn*w->bsize;
    if(at>=bytes)return 0u;
    /* FFS indirect data blocks are always allocated at full block size,
     * including a partially populated final logical block. */
    if(lbn>=12u)return w->frag;
    uint64_t n=bytes-at;
    if(n>w->bsize)n=w->bsize;
    return (uint32_t)((n+w->fsize-1u)/w->fsize);
}
static uint64_t max_file(const Ufs2Mutation *w)
{return (12ull+w->bsize/8u)*w->bsize;}
static int valid_run(const Ufs2Mutation *w,uint64_t at,uint32_t n)
{
    return n && n<=w->frag && at>w->root && at<w->total &&
        n<=w->total-at && at%w->frag+n<=w->frag;
}
static int mark_claim(const Ufs2Mutation *w,uint8_t *claimed,uint64_t at,uint32_t n)
{
    if(!valid_run(w,at,n))return IFS_FS_CORRUPT;
    for(uint32_t i=0;i<n;++i) {
        uint32_t x=(uint32_t)at+i;
        if(bit(claimed,x)||bit(w->cg+CG_FREE,x))return IFS_FS_CORRUPT;
        setbit(claimed,x,1u);
    }
    return IFS_FS_OK;
}
static void count_free(const Ufs2Mutation *w,const uint8_t *cg,Counts *c)
{
    memset(c,0,sizeof(*c));
    for(uint32_t i=0;i<INODES;++i)c->inodes+=!bit(cg+CG_IUSED,i);
    for(uint32_t b=0;b<w->total;b+=w->frag) {
        uint32_t free=0u,run=0u;
        uint32_t runs[8]={0};
        for(uint32_t i=0;i<w->frag;++i) {
            if(bit(cg+CG_FREE,b+i)){++free;++run;}
            else if(run){++runs[run];run=0;}
        }
        if(free==w->frag){++c->blocks;continue;}
        if(run)++runs[run];
        c->frags+=free;
        for(uint32_t i=1u;i<w->frag;++i)c->runs[i]+=runs[i];
    }
}
static int check_counts(const Ufs2Mutation *w,const uint8_t summary[16])
{
    const Counts *c=&w->counts;
    if(get32(w->cg+24)!=1u || get32(w->cg+28)!=c->blocks ||
       get32(w->cg+32)!=c->inodes || get32(w->cg+36)!=c->frags ||
       get32(summary)!=1u || get32(summary+4)!=c->blocks ||
       get32(summary+8)!=c->inodes || get32(summary+12)!=c->frags ||
       get64(w->sb+1008)!=1u || get64(w->sb+1016)!=c->blocks ||
       get64(w->sb+1024)!=c->inodes || get64(w->sb+1032)!=c->frags ||
       get32(w->sb+192)!=1u || get32(w->sb+196)!=c->blocks ||
       get32(w->sb+200)!=c->inodes || get32(w->sb+204)!=c->frags)
        return IFS_FS_CORRUPT;
    for(uint32_t i=1;i<w->frag;++i)
        if(get32(w->cg+52u+4u*i)!=c->runs[i])return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int validate_inode(Ufs2Mutation *w,uint32_t n,uint8_t *claimed,uint8_t *ind)
{
    const uint8_t *ino=w->inodes+n*INODE_BYTES;
    uint64_t size=get64(ino+16),sectors=0;
    if((get16(ino)&0170000u)!=0100000u || get16(ino+2)!=1u ||
       get32(ino+12)!=w->bsize || size>max_file(w) ||
       get32(ino+84)||get32(ino+88)||get32(ino+92)||
       get64(ino+96)||get64(ino+104)||get64(ino+216)||get64(ino+224)||
       get32(ino+240)||get32(ino+244))return IFS_FS_UNSUPPORTED;
    uint32_t blocks=blocks_for(w,size);
    uint64_t indirect=get64(ino+208);
    if(blocks>12u) {
        int rc=mark_claim(w,claimed,indirect,w->frag);
        if(rc!=IFS_FS_OK)return rc;
        rc=rd(w,disk(w,(uint32_t)indirect),ind,w->bsize);
        if(rc!=IFS_FS_OK)return rc;
        sectors=w->bsize/512u;
    } else if(indirect)return IFS_FS_CORRUPT;
    for(uint32_t i=0;i<12u;++i) {
        uint64_t at=get64(ino+112u+8u*i);
        if(i>=blocks){if(at)return IFS_FS_CORRUPT;continue;}
        uint32_t f=frags_for(w,size,i);
        int rc=mark_claim(w,claimed,at,f);
        if(rc!=IFS_FS_OK)return rc;
        sectors+=(uint64_t)f*w->fsize/512u;
    }
    if(blocks>12u)for(uint32_t i=0;i<w->bsize/8u;++i) {
        uint64_t at=get64(ind+8u*i);
        if(i+12u>=blocks){if(at)return IFS_FS_CORRUPT;continue;}
        uint32_t f=frags_for(w,size,i+12u);
        int rc=mark_claim(w,claimed,at,f);
        if(rc!=IFS_FS_OK)return rc;
        sectors+=(uint64_t)f*w->fsize/512u;
    }
    return sectors==get64(ino+24)?IFS_FS_OK:IFS_FS_CORRUPT;
}
static uint32_t min_record(uint32_t n){return (9u+n+3u)&~3u;}
static int root_name(const char *path,const char **name,uint32_t *length)
{
    if(!path || path[0]!='/')return IFS_FS_INVALID;
    uint32_t n=0;
    while(path[1u+n] && n<255u){if(path[1u+n]=='/')return IFS_FS_UNSUPPORTED;++n;}
    if(!n || path[1u+n] || (n==1u && path[1]=='.') ||
       (n==2u && path[1]=='.' && path[2]=='.'))return IFS_FS_INVALID;
    *name=path+1;*length=n;return IFS_FS_OK;
}
/* The writable profile admits one real root directory with unique, flat
 * regular-file entries. All its allocated inodes must be reachable exactly
 * once, and all allocated fragments must be explained by the inode tree. */
static int validate_namespace(Ufs2Mutation *w)
{
    uint8_t seen[INODES/8u]={0};
    const uint8_t *root=w->inodes+2u*INODE_BYTES;
    if((get16(root)&0170000u)!=0040000u || get16(root+2)!=2u ||
       get32(root+12)!=w->bsize || get64(root+16)!=DIR_BYTES ||
       get64(root+24)!=w->fsize/512u || get64(root+112)!=w->root ||
       get32(root+84)||get32(root+88)||get32(root+92)||get64(root+96)||
       get64(root+104)||get32(root+244))return IFS_FS_UNSUPPORTED;
    for(unsigned i=1;i<15u;++i)if(get64(root+112u+8u*i))return IFS_FS_CORRUPT;
    for(uint32_t pos=0;pos<DIR_BYTES;) {
        const uint8_t *e=w->dir+pos;
        if(DIR_BYTES-pos<8u)return IFS_FS_CORRUPT;
        uint32_t ino=get32(e),rec=get16(e+4),n=e[7];
        if(!n || rec<min_record(n) || (rec&3u) || rec>DIR_BYTES-pos ||
           e[8u+n]!=0u || ino>=INODES || !bit(w->cg+CG_IUSED,ino))
            return IFS_FS_CORRUPT;
        for(uint32_t i=0;i<n;++i)if(!e[8u+i]||e[8u+i]=='/')return IFS_FS_CORRUPT;
        if(!pos) {
            if(ino!=2u || e[6]!=4u || n!=1u || e[8]!='.' || rec!=min_record(1u))
                return IFS_FS_CORRUPT;
        } else if(pos==min_record(1u)) {
            if(ino!=2u || e[6]!=4u || n!=2u || e[8]!='.' || e[9]!='.')
                return IFS_FS_CORRUPT;
        } else {
            if(ino<3u || e[6]!=8u || bit(seen,ino) ||
               (n==1u&&e[8]=='.') || (n==2u&&e[8]=='.'&&e[9]=='.'))
                return IFS_FS_CORRUPT;
            for(uint32_t prev=0;prev<pos;prev+=get16(w->dir+prev+4u))
                if(w->dir[prev+7u]==n && !memcmp(w->dir+prev+8u,e+8u,n))
                    return IFS_FS_CORRUPT;
            setbit(seen,ino,1u);
        }
        pos+=rec;
    }
    if(!bit(w->cg+CG_IUSED,0u)||!bit(w->cg+CG_IUSED,1u)||
       !bit(w->cg+CG_IUSED,2u))return IFS_FS_CORRUPT;
    size_t bytes=(w->total+7u)/8u;
    uint8_t *claimed=w->host->allocate(w->host->context,bytes);
    uint8_t *ind=w->host->allocate(w->host->context,w->bsize);
    int rc=IFS_FS_NO_MEMORY;
    if(!claimed || !ind)goto done;
    memset(claimed,0,bytes);
    for(uint32_t i=0;i<=w->root;++i) {
        if(bit(w->cg+CG_FREE,i)){rc=IFS_FS_CORRUPT;goto done;}
        setbit(claimed,i,1u);
    }
    for(uint32_t i=3u;i<INODES;++i) {
        if(bit(seen,i)!=bit(w->cg+CG_IUSED,i)) {rc=IFS_FS_CORRUPT;goto done;}
        if(bit(seen,i)) {
            rc=validate_inode(w,i,claimed,ind);
            if(rc!=IFS_FS_OK)goto done;
        } else if(get16(w->inodes+i*INODE_BYTES)) {
            rc=IFS_FS_UNSUPPORTED;goto done;
        }
    }
    rc=IFS_FS_OK;
    for(uint32_t i=0;i<w->total;++i)
        if(bit(claimed,i)==bit(w->cg+CG_FREE,i)){rc=IFS_FS_CORRUPT;break;}
done:
    if(claimed)w->host->release(w->host->context,claimed);
    if(ind)w->host->release(w->host->context,ind);
    return rc;
}
int ufs2_mutation_open(const IfsFsHostRuntimeV2 *host,const LdUfsSummary *s,Ufs2Mutation **out)
{
    if(!out || !s || !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *out=NULL;
    if(host->capabilities&IFS_FS_HOST_READ_ONLY)return IFS_FS_ACCESS;
    const int large=s->block_size==65536u && s->fragment_size==8192u;
    const int small=s->block_size==16384u && s->fragment_size==4096u;
    if((!small&&!large)||s->variant!=LD_UFS_VARIANT_UFS2_LE ||
       !s->clean || s->flags || s->metadata_check_hashes ||
       s->contiguous_summary_size || s->pending_blocks || s->pending_inodes ||
       s->snapshot_count || s->cylinder_groups!=1u || s->inodes_per_group!=INODES ||
       s->superblock_offset!=SB_PRIMARY || s->filesystem_fragments!=s->fragments_per_group ||
       s->filesystem_fragments>262144u ||
       s->filesystem_fragments*s->fragment_size!=host->volume_bytes ||
       host->logical_sector_bytes>s->fragment_size ||
       !s->fragments_per_block || s->fragments_per_block!=s->block_size/s->fragment_size ||
       s->filesystem_fragments%s->fragments_per_block)return IFS_FS_ACCESS;
    uint32_t frag=s->fragments_per_block;
    uint32_t sblk=(SB_PRIMARY+SB_BYTES+s->block_size-1u)/s->block_size*frag;
    uint32_t cblk=sblk+frag,iblk=cblk+frag;
    uint32_t dblk=iblk+INODES*INODE_BYTES/s->fragment_size;
    uint32_t total=(uint32_t)s->filesystem_fragments;
    uint32_t cgsize=(CG_FREE+(total+7u)/8u+s->fragment_size-1u)/s->fragment_size*s->fragment_size;
    if(s->cylinder_block_fragment!=cblk || s->inode_block_fragment!=iblk ||
       s->first_data_fragment!=dblk || s->cylinder_group_size!=cgsize ||
       cgsize>s->block_size || total<=dblk+2u || s->data_fragments!=total-dblk-1u)
        return IFS_FS_ACCESS;
    Ufs2Mutation *w=host->allocate(host->context,sizeof(*w));
    if(!w)return IFS_FS_NO_MEMORY;
    memset(w,0,sizeof(*w));
    w->host=host;w->bsize=s->block_size;w->fsize=s->fragment_size;
    w->frag=frag;w->total=total;w->cgsize=cgsize;
    w->sblk=sblk;w->cblk=cblk;w->iblk=iblk;w->dblk=dblk;w->root=dblk+1u;
    uint8_t backup[SB_BYTES],summary[16];
    int rc=rd(w,SB_PRIMARY,w->sb,SB_BYTES);
    if(rc==IFS_FS_OK)rc=rd(w,disk(w,sblk),backup,SB_BYTES);
    if(rc==IFS_FS_OK)rc=rd(w,disk(w,cblk),w->cg,cgsize);
    if(rc==IFS_FS_OK)rc=rd(w,disk(w,iblk),w->inodes,sizeof(w->inodes));
    if(rc==IFS_FS_OK)rc=rd(w,disk(w,dblk),summary,sizeof(summary));
    if(rc==IFS_FS_OK)rc=rd(w,disk(w,w->root),w->dir,DIR_BYTES);
    if(rc!=IFS_FS_OK)goto fail;
    if(get64(backup+992)!=disk(w,sblk)||get64(w->sb+992)!=SB_PRIMARY) {
        rc=IFS_FS_ACCESS;goto fail;
    }
    put64(backup+992,SB_PRIMARY);
    if(memcmp(backup,w->sb,1376u)||w->sb[209]!=1u||
       get32(w->sb+1372)!=UFS2_MAGIC||get32(w->sb+8)!=sblk||get64(w->sb+1000)!=SB_PRIMARY||
       get64(w->sb+1096)!=dblk||get32(w->cg+4)!=CG_MAGIC||
       get32(w->cg+12)!=0u||get32(w->cg+20)!=total||
       get32(w->cg+92)!=CG_IUSED||get32(w->cg+96)!=CG_FREE||
       get32(w->cg+100)!=CG_FREE+(total+7u)/8u||
       get32(w->cg+116)!=INODES||get32(w->cg+120)!=INODES) {
        rc=IFS_FS_ACCESS;goto fail;
    }
    count_free(w,w->cg,&w->counts);
    rc=check_counts(w,summary);
    if(rc==IFS_FS_OK)rc=validate_namespace(w);
    if(rc!=IFS_FS_OK)goto fail;
    *out=w;return IFS_FS_OK;
fail:
    host->release(host->context,w);return rc;
}
int ufs2_mutation_faulted(const Ufs2Mutation *w){return w&&w->faulted;}
void ufs2_mutation_counts(const Ufs2Mutation *w,uint64_t *blocks,uint64_t *frags)
{*blocks=w->counts.blocks;*frags=w->counts.frags;}
void ufs2_mutation_close(Ufs2Mutation *w)
{if(w)w->host->release(w->host->context,w);}

static int write_super(Ufs2Mutation *w,int clean,int primary)
{
    uint64_t at=primary?SB_PRIMARY:disk(w,w->sblk);
    w->sb[209]=(uint8_t)clean;put64(w->sb+992,at);
    int rc=wr(w,at,w->sb,SB_BYTES);
    put64(w->sb+992,SB_PRIMARY);
    return rc;
}
static int poison(Ufs2Mutation *w,int rc)
{
    w->faulted=1;
    /* Do not let a failed final clean barrier masquerade as a committed
     * operation. Best-effort re-marking cannot turn failure into success. */
    (void)write_super(w,0,1);(void)write_super(w,0,0);(void)barrier(w);
    return rc==IFS_FS_OK?IFS_FS_IO_ERROR:rc;
}
static int begin(Ufs2Mutation *w)
{
    if(w->faulted)return IFS_FS_IO_ERROR;
    if(cancelled(w))return IFS_FS_BUSY;
    int rc=write_super(w,0,1);
    if(rc==IFS_FS_OK)rc=barrier(w);
    if(rc==IFS_FS_OK)rc=write_super(w,0,0);
    if(rc==IFS_FS_OK)rc=barrier(w);
    return rc==IFS_FS_OK?rc:poison(w,rc);
}
static int finish(Ufs2Mutation *w)
{
    /* All data and inode/directory pointers have been written under the
     * dirty superblocks. Publish matching CG and both summary copies,
     * flush them, then publish clean backup and finally clean primary. */
    count_free(w,w->cg,&w->counts);
    Counts *c=&w->counts;
    uint8_t summary[16];
    put32(summary,1u);put32(summary+4,c->blocks);
    put32(summary+8,c->inodes);put32(summary+12,c->frags);
    memcpy(w->cg+24,summary,sizeof(summary));
    memcpy(w->sb+192,summary,sizeof(summary));
    for(uint32_t i=1;i<8u;++i)put32(w->cg+52u+4u*i,c->runs[i]);
    put64(w->sb+1008,1u);put64(w->sb+1016,c->blocks);
    put64(w->sb+1024,c->inodes);put64(w->sb+1032,c->frags);
    uint64_t seconds=now(w);
    put64(w->cg+136,seconds);put64(w->sb+1072,seconds);
    put32(w->sb+32,(uint32_t)seconds);
    put32(w->sb+1352,0x7c269d38u-(uint32_t)seconds);
    int rc=wr(w,disk(w,w->cblk),w->cg,w->cgsize);
    if(rc==IFS_FS_OK)rc=wr(w,disk(w,w->dblk),summary,sizeof(summary));
    if(rc==IFS_FS_OK)rc=write_super(w,0,0);
    if(rc==IFS_FS_OK)rc=write_super(w,0,1);
    if(rc==IFS_FS_OK)rc=barrier(w);
    if(rc==IFS_FS_OK)rc=write_super(w,1,0);
    if(rc==IFS_FS_OK)rc=barrier(w);
    if(rc==IFS_FS_OK)rc=write_super(w,1,1);
    if(rc==IFS_FS_OK)rc=barrier(w);
    return rc==IFS_FS_OK?rc:poison(w,rc);
}
static int lookup(const Ufs2Mutation *w,const char *name,uint32_t n,uint32_t *ino)
{
    for(uint32_t p=0;p<DIR_BYTES;p+=get16(w->dir+p+4u))
        if(w->dir[p+7u]==n && !memcmp(w->dir+p+8u,name,n)) {
            *ino=get32(w->dir+p);return IFS_FS_OK;
        }
    return IFS_FS_NOT_FOUND;
}
int ufs2_mutation_create(Ufs2Mutation *w,const char *path,uint32_t type)
{
    if(!w || w->faulted)return IFS_FS_IO_ERROR;
    if(type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    const char *name;uint32_t n,ino=0;
    int rc=root_name(path,&name,&n);
    if(rc!=IFS_FS_OK)return rc;
    if(lookup(w,name,n,&ino)==IFS_FS_OK)return IFS_FS_EXISTS;
    for(ino=3u;ino<INODES && bit(w->cg+CG_IUSED,ino);++ino){}
    if(ino==INODES)return IFS_FS_NO_SPACE;
    uint32_t pos=0,used=0,rec=0;
    for(;pos<DIR_BYTES;pos+=rec) {
        rec=get16(w->dir+pos+4u);used=min_record(w->dir[pos+7u]);
        if(rec-used>=min_record(n))break;
    }
    if(pos==DIR_BYTES)return IFS_FS_NO_SPACE;
    uint8_t inode[INODE_BYTES]={0},dir[DIR_BYTES],root[INODE_BYTES];
    put16(inode,0100644u);put16(inode+2,1u);put32(inode+12,w->bsize);
    uint64_t seconds=now(w);
    for(uint32_t i=32u;i<=56u;i+=8u)put64(inode+i,seconds);
    if(w->host->structure_bytes>=sizeof(*w->host) && w->host->random_bytes) {
        uint32_t generation;
        rc=ifs_fs_host_random_bytes_v2(w->host,&generation,sizeof(generation));
        if(rc!=IFS_FS_OK)return rc;
        put32(inode+80,generation);
    }
    memcpy(dir,w->dir,sizeof(dir));
    put16(dir+pos+4u,used);
    uint8_t *entry=dir+pos+used;
    memset(entry,0,rec-used);put32(entry,ino);put16(entry+4,rec-used);
    entry[6]=8u;entry[7]=(uint8_t)n;memcpy(entry+8,name,n);
    memcpy(root,w->inodes+2u*INODE_BYTES,sizeof(root));
    put64(root+40,seconds);put64(root+48,seconds);
    rc=begin(w);if(rc!=IFS_FS_OK)return rc;
    rc=wr(w,inode_disk(w,ino),inode,sizeof(inode));
    if(rc==IFS_FS_OK)rc=wr(w,disk(w,w->root),dir,sizeof(dir));
    if(rc==IFS_FS_OK)rc=wr(w,inode_disk(w,2u),root,sizeof(root));
    if(rc!=IFS_FS_OK)return poison(w,rc);
    setbit(w->cg+CG_IUSED,ino,1u);
    memcpy(w->inodes+ino*INODE_BYTES,inode,sizeof(inode));
    memcpy(w->inodes+2u*INODE_BYTES,root,sizeof(root));
    memcpy(w->dir,dir,sizeof(dir));
    return finish(w);
}
/* A full data block is block-aligned; partial allocations never cross a
 * block boundary. Prefer partially free blocks for fragments so they do
 * not unnecessarily consume entirely free full-block allocation runs. */
static uint32_t reserve(const Ufs2Mutation *w,uint8_t *cg,uint32_t n)
{
    uint8_t *map=cg+CG_FREE;
    for(unsigned pass=0;pass<(n==w->frag?1u:2u);++pass)
        for(uint32_t b=(w->root/w->frag)*w->frag;b<w->total;b+=w->frag) {
            unsigned free=0;
            for(uint32_t j=0;j<w->frag;++j)free+=bit(map,b+j);
            if(free<n || (n<w->frag && !pass && free==w->frag))continue;
            for(uint32_t j=0;j+n<=w->frag;++j) {
                if(b+j<=w->root)continue;
                uint32_t k=0;while(k<n && bit(map,b+j+k))++k;
                if(k!=n)continue;
                for(k=0;k<n;++k)setbit(map,b+j+k,0u);
                return b+j;
            }
        }
    return 0u;
}
static uint8_t *pointer(uint8_t *inode,uint8_t *ind,uint32_t lbn)
{return lbn<12u?inode+112u+8u*lbn:ind+8u*(lbn-12u);}
int ufs2_mutation_write(Ufs2Mutation *w,const char *path,uint64_t offset,
                        const void *source,uint32_t length)
{
    if(!w || w->faulted)return IFS_FS_IO_ERROR;
    if(!source && length)return IFS_FS_INVALID;
    const char *name;uint32_t n,number=0;
    int rc=root_name(path,&name,&n);
    if(rc!=IFS_FS_OK)return rc;
    rc=lookup(w,name,n,&number);if(rc!=IFS_FS_OK)return rc;
    uint8_t inode[INODE_BYTES];
    memcpy(inode,w->inodes+number*INODE_BYTES,sizeof(inode));
    uint64_t oldsize=get64(inode+16);
    if(offset>oldsize || offset>max_file(w) || length>max_file(w)-offset)
        return IFS_FS_UNSUPPORTED; /* no sparse growth or double indirect */
    if(!length)return IFS_FS_OK;
    uint64_t end=offset+length,newsize=end>oldsize?end:oldsize;
    uint32_t first=(uint32_t)(offset/w->bsize),last=(uint32_t)((end-1u)/w->bsize);
    uint32_t plans=last-first+1u;
    uint8_t *cg=w->host->allocate(w->host->context,w->cgsize);
    uint8_t *ind=w->host->allocate(w->host->context,w->bsize);
    uint8_t *data=w->host->allocate(w->host->context,w->bsize);
    BlockPlan *plan=w->host->allocate(w->host->context,(size_t)plans*sizeof(*plan));
    if(!cg||!ind||!data||!plan){rc=IFS_FS_NO_MEMORY;goto done;}
    memcpy(cg,w->cg,w->cgsize);memset(ind,0,w->bsize);
    uint32_t indirect=(uint32_t)get64(inode+208);
    if(indirect) {
        rc=rd(w,disk(w,indirect),ind,w->bsize);
        if(rc!=IFS_FS_OK)goto done;
    } else if(last>=12u) {
        indirect=reserve(w,cg,w->frag);
        if(!indirect){rc=IFS_FS_NO_SPACE;goto done;}
        put64(inode+208,indirect);
    }
    /* Stage every allocation before touching the medium. Released source
     * fragments remain reserved until all destinations have been chosen. */
    for(uint32_t i=0;i<plans;++i) {
        uint32_t lbn=first+i,oldn=frags_for(w,oldsize,lbn),need=frags_for(w,newsize,lbn);
        uint8_t *ptr=pointer(inode,ind,lbn);
        uint32_t old=(uint32_t)get64(ptr),at=old;
        if(oldn && !valid_run(w,old,oldn)){rc=IFS_FS_CORRUPT;goto done;}
        if(!oldn && old){rc=IFS_FS_CORRUPT;goto done;}
        if(need>oldn) {
            int extend=oldn && old%w->frag+need<=w->frag;
            for(uint32_t j=oldn;extend && j<need;++j)
                if(!bit(cg+CG_FREE,old+j))extend=0;
            if(extend)for(uint32_t j=oldn;j<need;++j)setbit(cg+CG_FREE,old+j,0u);
            else {
                at=reserve(w,cg,need);
                if(!at){rc=IFS_FS_NO_SPACE;goto done;}
            }
            put64(ptr,at);
        }
        plan[i]=(BlockPlan){old,oldn,at,need};
    }
    for(uint32_t i=0;i<plans;++i)if(plan[i].oldn && plan[i].old!=plan[i].at)
        for(uint32_t j=0;j<plan[i].oldn;++j)setbit(cg+CG_FREE,plan[i].old+j,1u);
    put64(inode+16,newsize);
    uint64_t allocated=indirect?(uint64_t)(blocks_for(w,newsize)+1u)*w->bsize:
        ((newsize+w->fsize-1u)/w->fsize)*w->fsize;
    put64(inode+24,allocated/512u);
    uint64_t seconds=now(w);put64(inode+40,seconds);put64(inode+48,seconds);
    rc=begin(w);if(rc!=IFS_FS_OK)goto done;
    for(uint32_t i=0;i<plans;++i) {
        BlockPlan *p=&plan[i];
        memset(data,0,w->bsize);
        if(p->oldn) {
            rc=rd(w,disk(w,p->old),data,p->oldn*w->fsize);
            if(rc!=IFS_FS_OK)break;
        }
        uint64_t base=(uint64_t)(first+i)*w->bsize;
        uint64_t lo=offset>base?offset:base,hi=end<base+w->bsize?end:base+w->bsize;
        memcpy(data+(size_t)(lo-base),(const uint8_t *)source+(size_t)(lo-offset),(size_t)(hi-lo));
        uint64_t used=newsize-base;
        if(used<(uint64_t)p->n*w->fsize)
            memset(data+(size_t)used,0,(size_t)((uint64_t)p->n*w->fsize-used));
        rc=wr(w,disk(w,p->at),data,p->n*w->fsize);
        if(rc!=IFS_FS_OK)break;
    }
    if(rc==IFS_FS_OK)rc=barrier(w);
    if(rc==IFS_FS_OK && indirect)rc=wr(w,disk(w,indirect),ind,w->bsize);
    if(rc==IFS_FS_OK)rc=wr(w,inode_disk(w,number),inode,sizeof(inode));
    if(rc!=IFS_FS_OK){rc=poison(w,rc);goto done;}
    memcpy(w->cg,cg,w->cgsize);
    memcpy(w->inodes+number*INODE_BYTES,inode,sizeof(inode));
    rc=finish(w);
done:
    if(cg)w->host->release(w->host->context,cg);
    if(ind)w->host->release(w->host->context,ind);
    if(data)w->host->release(w->host->context,data);
    if(plan)w->host->release(w->host->context,plan);
    return rc;
}
