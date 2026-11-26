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
#include <openssl/sha.h>

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
typedef struct {uint32_t inode,first,fragments,reserve,indirect;uint64_t size;int relocate;} Ufs2Placement;
struct Ufs2RelocationPlan {
    uint32_t policy,count,journal_first,journal_fragments;
    int completed;
    Ufs2Placement entries[INODES];
};

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
static void prepare_counts(Ufs2Mutation *w,uint8_t summary[16])
{
    count_free(w,w->cg,&w->counts);
    Counts *c=&w->counts;
    put32(summary,1u);put32(summary+4,c->blocks);
    put32(summary+8,c->inodes);put32(summary+12,c->frags);
    memcpy(w->cg+24,summary,16u);
    memcpy(w->sb+192,summary,16u);
    for(uint32_t i=1;i<8u;++i)put32(w->cg+52u+4u*i,c->runs[i]);
    put64(w->sb+1008,1u);put64(w->sb+1016,c->blocks);
    put64(w->sb+1024,c->inodes);put64(w->sb+1032,c->frags);
    uint64_t seconds=now(w);
    put64(w->cg+136,seconds);put64(w->sb+1072,seconds);
    put32(w->sb+32,(uint32_t)seconds);
    put32(w->sb+1352,0x7c269d38u-(uint32_t)seconds);
}
static int finish(Ufs2Mutation *w)
{
    uint8_t summary[16];prepare_counts(w,summary);
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

/* Private native redo format for the non-bootable, formatter-owned UFS2
 * profile. The first boot sector is temporary recovery metadata ONLY
 * when the entire 8 KiB boot area was zero. It is restored to zero after
 * publication. The journal is in a free run disjoint from all placements
 * and contains both complete before and after images of fixed metadata.
 * All geometry is independently derived from the host, never the record. */
enum { REDO_RANGES=5, REDO_BOOT=8192, REDO_HEADER=80 };
static const uint8_t redo_magic[8]={'I','F','S','2','R','E','D','O'};
typedef struct {
    uint32_t fsize,bsize,total,half,bytes,fragments;
    uint64_t at[REDO_RANGES];
    uint32_t len[REDO_RANGES],pos[REDO_RANGES];
} RedoLayout;
static int redo_layout(const IfsFsHostRuntimeV2 *h,RedoLayout *l)
{
    if(!ifs_fs_host_v2_valid(h) || h->volume_bytes<16ull*1024u*1024u ||
       h->volume_bytes>2ull*1024u*1024u*1024u)return IFS_FS_UNSUPPORTED;
    const int large=h->volume_bytes>128ull*1024u*1024u;
    const uint32_t b=large?65536u:16384u,f=large?8192u:4096u;
    const uint32_t ipg=INODES,frag=b/f;
    if(h->volume_bytes%b || h->logical_sector_bytes>f ||
       h->physical_sector_bytes!=h->logical_sector_bytes)
        return IFS_FS_UNSUPPORTED;
    const uint32_t total=(uint32_t)(h->volume_bytes/f);
    const uint32_t sblk=(SB_PRIMARY+SB_BYTES+b-1u)/b*frag;
    const uint32_t cblk=sblk+frag,iblk=cblk+frag;
    const uint32_t dblk=iblk+ipg*INODE_BYTES/f;
    const uint32_t freeoff=CG_FREE;
    const uint32_t cgsize=(freeoff+(total+7u)/8u+f-1u)/f*f;
    if(cgsize>b || total<=dblk+2u)return IFS_FS_UNSUPPORTED;
    *l=(RedoLayout){.fsize=f,.bsize=b,.total=total,
        .at={SB_PRIMARY,(uint64_t)sblk*f,(uint64_t)cblk*f,(uint64_t)iblk*f,(uint64_t)dblk*f},
        .len={SB_BYTES,SB_BYTES,cgsize,ipg*INODE_BYTES,f}};
    for(unsigned i=0;i<REDO_RANGES;++i){l->pos[i]=l->half;l->half+=l->len[i];}
    l->bytes=2u*l->half;l->fragments=l->bytes/f;
    return IFS_FS_OK;
}
static int redo_zero(const uint8_t *p,uint32_t n)
{
    for(uint32_t i=0;i<n;++i)if(p[i])return 0;
    return 1;
}
static int redo_header(const IfsFsHostRuntimeV2 *h,RedoLayout *l,
                       uint8_t marker[REDO_BOOT],uint32_t *first)
{
    int rc=ifs_fs_host_read_bytes_v2(h,0u,marker,REDO_BOOT);
    if(rc!=IFS_FS_OK)return rc;
    if(memcmp(marker,redo_magic,sizeof(redo_magic)))
        return !memcmp(marker,redo_magic,4u)?IFS_FS_CORRUPT:IFS_FS_NOT_FOUND;
    rc=redo_layout(h,l);if(rc!=IFS_FS_OK)return rc;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    if(!SHA256(marker,48u,digest) || memcmp(digest,marker+48u,sizeof(digest)) ||
       !redo_zero(marker+REDO_HEADER,REDO_BOOT-REDO_HEADER))
        return IFS_FS_CORRUPT;
    *first=get32(marker+8u);
    if(get32(marker+12u)!=l->bytes || !*first || *first>=l->total ||
       l->fragments>l->total-*first ||
       (uint64_t)*first*l->fsize<l->at[4]+2ull*l->fsize)
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
int ufs2_relocation_marker(const IfsFsHostRuntimeV2 *h)
{
    RedoLayout l;uint8_t marker[REDO_BOOT];uint32_t first;
    return redo_header(h,&l,marker,&first);
}
static uint32_t placement_run(const Ufs2Mutation *w,const uint8_t *map,uint32_t need)
{
    uint32_t run=0u;
    for(uint32_t i=w->root+1u;i<w->total;++i) {
        if(!(i%4096u) && cancelled(w))return 0u;
        if(!bit(map,i)){run=0u;continue;}
        if(!run && i%w->frag)continue;
        if(++run==need)return i+1u-need;
    }
    return 0u;
}
static void placement_take(uint8_t *map,uint32_t first,uint32_t count)
{
    for(uint32_t i=0;i<count;++i)setbit(map,first+i,0u);
}

/* Plan against an independent copy of the authoritative native fragment
 * bitmap. A destination is entirely free before publication: no overlap
 * with any existing inode, indirect block, or another planned growth gap.
 * This conservative placement does not promise in-place compaction. */
static int placement_free(const Ufs2Mutation *w,const uint8_t *map,
                          uint32_t first,uint32_t count)
{
    if(first>=w->total || count>w->total-first)return 0;
    for(uint32_t i=0;i<count;++i)if(!bit(map,first+i))return 0;
    return 1;
}
int ufs2_relocation_plan(Ufs2Mutation *w,const char *path,uint32_t policy,
                         Ufs2RelocationPlan **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!w || !path || (policy!=0u && policy!=10u))return IFS_FS_INVALID;
    if(w->faulted)return IFS_FS_IO_ERROR;
    if(cancelled(w))return IFS_FS_BUSY;
    uint32_t selected=0u;
    if(strcmp(path,"/")) {
        const char *name;uint32_t n;
        int rc=root_name(path,&name,&n);
        if(rc!=IFS_FS_OK)return rc;
        rc=lookup(w,name,n,&selected);
        if(rc!=IFS_FS_OK)return rc;
        if(selected<3u)return IFS_FS_INVALID;
    }
    Ufs2RelocationPlan *p=w->host->allocate(w->host->context,sizeof(*p));
    uint8_t *map=w->host->allocate(w->host->context,(w->total+7u)/8u);
    uint8_t *ind=w->host->allocate(w->host->context,w->bsize);
    int rc=IFS_FS_NO_MEMORY;
    if(!p || !map || !ind)goto done;
    memset(p,0,sizeof(*p));p->policy=policy;
    memcpy(map,w->cg+CG_FREE,(w->total+7u)/8u);
    for(uint32_t n=3u;n<INODES;++n) {
        if(selected && selected!=n)continue;
        if(!bit(w->cg+CG_IUSED,n))continue;
        if(cancelled(w)){rc=IFS_FS_BUSY;goto done;}
        const uint8_t *ino=w->inodes+n*INODE_BYTES;
        const uint64_t size=get64(ino+16);
        if(!size)continue;
        uint32_t count=blocks_for(w,size);
        if(count>12u) {
            rc=rd(w,disk(w,(uint32_t)get64(ino+208)),ind,w->bsize);
            if(rc!=IFS_FS_OK)goto done;
        }
        uint32_t first=(uint32_t)get64(ino+112),end=first,used=0u;
        int packed=1;
        for(uint32_t j=0;j<count;++j) {
            uint64_t at=j<12u?get64(ino+112u+8u*j):get64(ind+8u*(j-12u));
            uint32_t len=frags_for(w,size,j);
            if(!valid_run(w,at,len)){rc=IFS_FS_CORRUPT;goto done;}
            if(at!=end)packed=0;
            end=(uint32_t)at+len;used+=len;
        }
        /* The public allocation unit is the full UFS block. Round ten
         * percent of the allocated logical blocks upward, including tiny
         * files, and reserve that many whole blocks of free fragments.
         * Slack within the file is not free space outside the file. */
        uint32_t gap=policy?(count/10u+(count%10u!=0u))*w->frag:0u;
        if(packed && gap && !placement_free(w,map,end,gap))packed=0;
        if(!packed) {
            uint32_t need=used+gap;
            first=placement_run(w,map,need);
            if(!first){rc=cancelled(w)?IFS_FS_BUSY:IFS_FS_NO_SPACE;goto done;}
            placement_take(map,first,need);
        } else {
            for(uint32_t i=0;i<gap;++i)setbit(map,end+i,0u);
        }
        p->entries[p->count++]=(Ufs2Placement){n,first,used,gap,0u,size,!packed};
    }
    unsigned moves=0u;
    for(uint32_t i=0;i<p->count;++i) {
        Ufs2Placement *entry=&p->entries[i];
        if(!entry->relocate)continue;
        ++moves;
        if(blocks_for(w,entry->size)>12u) {
            entry->indirect=placement_run(w,map,w->frag);
            if(!entry->indirect){rc=cancelled(w)?IFS_FS_BUSY:IFS_FS_NO_SPACE;goto done;}
            placement_take(map,entry->indirect,w->frag);
        }
    }
    if(moves) {
        RedoLayout l;uint8_t boot[REDO_BOOT];
        rc=redo_layout(w->host,&l);
        if(rc!=IFS_FS_OK)goto done;
        rc=rd(w,0u,boot,sizeof(boot));
        if(rc!=IFS_FS_OK)goto done;
        if(!redo_zero(boot,sizeof(boot))){rc=IFS_FS_ACCESS;goto done;}
        p->journal_fragments=l.fragments;
        p->journal_first=placement_run(w,map,l.fragments);
        if(!p->journal_first){rc=cancelled(w)?IFS_FS_BUSY:IFS_FS_NO_SPACE;goto done;}
    }
    *out=p;p=NULL;rc=IFS_FS_OK;
done:
    if(ind)w->host->release(w->host->context,ind);
    if(map)w->host->release(w->host->context,map);
    if(p)w->host->release(w->host->context,p);
    return rc;
}
void ufs2_relocation_release(Ufs2Mutation *w,Ufs2RelocationPlan *p)
{
    if(w && p)w->host->release(w->host->context,p);
}

/* All callbacks are serialized by the module's operation gate. Overlay
 * only the fixed metadata ranges when independently reopening the journal
 * before/after images through the production native reader. Data and
 * indirect blocks must already exist durably at their real locations. */
static struct {
    const IfsFsHostRuntimeV2 *base;
    const RedoLayout *layout;
    const uint8_t *image;
} redo_overlay;
static int redo_read(void *context,uint64_t at,void *out,uint32_t bytes)
{
    if(!redo_overlay.base || redo_overlay.base->read_at(context,at,out,bytes))
        return -1;
    const RedoLayout *l=redo_overlay.layout;
    uint8_t *dst=out;
    for(unsigned i=0;i<REDO_RANGES;++i) {
        uint64_t lo=at>l->at[i]?at:l->at[i];
        uint64_t hi=at+bytes<l->at[i]+l->len[i]?at+bytes:l->at[i]+l->len[i];
        if(lo<hi)memcpy(dst+(size_t)(lo-at),redo_overlay.image+l->pos[i]+
            (size_t)(lo-l->at[i]),(size_t)(hi-lo));
    }
    return 0;
}
static int redo_open(const IfsFsHostRuntimeV2 *h,const RedoLayout *l,
                     const uint8_t *image,Ufs2Mutation **out)
{
    IfsFsHostRuntimeV2 overlay={0};
    size_t host_bytes=h->structure_bytes<sizeof(overlay)?h->structure_bytes:sizeof(overlay);
    memcpy(&overlay,h,host_bytes);overlay.structure_bytes=(uint32_t)host_bytes;
    overlay.read_at=redo_read;
    redo_overlay.base=h;redo_overlay.layout=l;redo_overlay.image=image;
    LdUfsSummary summary;
    int rc=ufs_read_summary_host_v2(&overlay,&summary);
    if(rc==IFS_FS_OK)rc=ufs2_mutation_open(&overlay,&summary,out);
    redo_overlay.base=NULL;redo_overlay.layout=NULL;redo_overlay.image=NULL;
    if(rc==IFS_FS_OK)(*out)->host=h;
    return rc;
}
/* A replay may encounter any mix of already-published and original
 * metadata, including interrupted logical-sector writes. Every observed
 * byte must belong to the recorded before/after state (or the temporary
 * dirty indication). Do not let an unrelated stale record rewrite media. */
static int redo_current_matches(const IfsFsHostRuntimeV2 *h,const RedoLayout *l,
                                const uint8_t *images)
{
    uint8_t *current=h->allocate(h->context,l->bsize);
    if(!current)return IFS_FS_NO_MEMORY;
    int rc=IFS_FS_OK;
    for(unsigned i=0;i<REDO_RANGES && rc==IFS_FS_OK;++i) {
        for(uint32_t offset=0u;offset<l->len[i];) {
            uint32_t n=l->len[i]-offset;
            if(n>l->bsize)n=l->bsize;
            rc=ifs_fs_host_read_bytes_v2(h,l->at[i]+offset,current,n);
            if(rc!=IFS_FS_OK)break;
            const uint8_t *old=images+l->pos[i]+offset;
            const uint8_t *next=old+l->half;
            for(uint32_t j=0;j<n;++j) {
                if(current[j]==old[j] || current[j]==next[j] ||
                   (i<2u && offset+j==209u && !current[j]))continue;
                rc=IFS_FS_CORRUPT;break;
            }
            offset+=n;
        }
    }
    h->release(h->context,current);return rc;
}
/* Before any replay write, both snapshots must independently describe the
 * same complete namespace. Every changed data or indirect pointer must
 * reference an initially free destination and byte-identical file data.
 * The temporary journal remains free in both allocation snapshots. */
static int redo_validate(const IfsFsHostRuntimeV2 *h,const RedoLayout *l,
                         const uint8_t *images,uint32_t first,
                         IfsFsVolumeInfoV2 *info)
{
    Ufs2Mutation *old=NULL,*next=NULL;
    int rc=redo_current_matches(h,l,images);
    if(rc==IFS_FS_OK)rc=redo_open(h,l,images,&old);
    if(rc==IFS_FS_OK)rc=redo_open(h,l,images+l->half,&next);
    if(rc!=IFS_FS_OK)goto done;
    if(old->bsize!=l->bsize || next->bsize!=l->bsize ||
       old->fsize!=l->fsize || next->fsize!=l->fsize ||
       old->total!=l->total || next->total!=l->total ||
       memcmp(old->dir,next->dir,DIR_BYTES) ||
       memcmp(old->cg+CG_IUSED,next->cg+CG_IUSED,INODES/8u) ||
       !placement_free(old,old->cg+CG_FREE,first,l->fragments) ||
       !placement_free(next,next->cg+CG_FREE,first,l->fragments) ||
       (uint64_t)old->counts.blocks*old->frag+old->counts.frags !=
       (uint64_t)next->counts.blocks*next->frag+next->counts.frags) {
        rc=IFS_FS_CORRUPT;goto done;
    }
    uint8_t normalized_sb[SB_BYTES];
    memcpy(normalized_sb,next->sb,SB_BYTES);
    /* Only count summaries and publication timestamps are mutable. */
    memcpy(normalized_sb+32u,old->sb+32u,4u);
    memcpy(normalized_sb+192u,old->sb+192u,16u);
    memcpy(normalized_sb+1008u,old->sb+1008u,32u);
    memcpy(normalized_sb+1072u,old->sb+1072u,8u);
    memcpy(normalized_sb+1352u,old->sb+1352u,4u);
    if(memcmp(normalized_sb,old->sb,SB_BYTES)) {
        rc=IFS_FS_CORRUPT;goto done;
    }
    uint8_t *buf=h->allocate(h->context,4u*l->bsize);
    if(!buf){rc=IFS_FS_NO_MEMORY;goto done;}
    uint8_t *oi=buf,*ni=buf+l->bsize,*od=buf+2u*l->bsize,*nd=buf+3u*l->bsize;
    for(uint32_t n=0;n<INODES;++n) {
        const uint8_t *a=old->inodes+n*INODE_BYTES,*b=next->inodes+n*INODE_BYTES;
        if(n<3u || !bit(old->cg+CG_IUSED,n)) {
            if(memcmp(a,b,INODE_BYTES)){rc=IFS_FS_CORRUPT;break;}
            continue;
        }
        if(memcmp(a,b,112u) || memcmp(a+216u,b+216u,INODE_BYTES-216u)) {
            rc=IFS_FS_CORRUPT;break;
        }
        const uint64_t size=get64(a+16u);
        const uint32_t blocks=blocks_for(old,size);
        if(blocks>12u) {
            const uint32_t x=(uint32_t)get64(a+208u),y=(uint32_t)get64(b+208u);
            if(x!=y && !placement_free(old,old->cg+CG_FREE,y,old->frag)) {
                rc=IFS_FS_CORRUPT;break;
            }
            rc=rd(old,disk(old,x),oi,old->bsize);
            if(rc==IFS_FS_OK)rc=rd(next,disk(next,y),ni,next->bsize);
            if(rc!=IFS_FS_OK)break;
        }
        for(uint32_t j=0;j<blocks;++j) {
            const uint32_t x=j<12u?get64(a+112u+8u*j):get64(oi+8u*(j-12u));
            const uint32_t y=j<12u?get64(b+112u+8u*j):get64(ni+8u*(j-12u));
            if(x==y)continue;
            const uint32_t count=frags_for(old,size,j);
            if(!placement_free(old,old->cg+CG_FREE,y,count)) {
                rc=IFS_FS_CORRUPT;break;
            }
            rc=rd(old,disk(old,x),od,count*old->fsize);
            if(rc==IFS_FS_OK)rc=rd(next,disk(next,y),nd,count*next->fsize);
            if(rc!=IFS_FS_OK)break;
            if(memcmp(od,nd,count*old->fsize)){rc=IFS_FS_CORRUPT;break;}
        }
        if(rc!=IFS_FS_OK)break;
    }
    h->release(h->context,buf);
    if(rc==IFS_FS_OK && info) {
        *info=(IfsFsVolumeInfoV2){.structure_bytes=sizeof(*info),
            .variant=LD_UFS_VARIANT_UFS2_LE,.max_name_bytes=255u,
            .block_bytes=next->bsize,.total_bytes=h->volume_bytes,
            .free_bytes=((uint64_t)next->counts.blocks*next->frag+next->counts.frags)*next->fsize};
    }
done:
    ufs2_mutation_close(old);ufs2_mutation_close(next);
    return rc;
}
static int redo_flush(const IfsFsHostRuntimeV2 *h)
{return h->flush(h->context)==0?IFS_FS_OK:IFS_FS_IO_ERROR;}
static int redo_apply(const IfsFsHostRuntimeV2 *h,const RedoLayout *l,const uint8_t *images)
{
    uint8_t sb[SB_BYTES],zero[REDO_BOOT]={0};
    const uint8_t *next=images+l->half;
    int rc=IFS_FS_OK;
    /* Preserve a conventional dirty indication while metadata is being
     * replaced. The locator remains durable until BOTH clean copies and
     * the entire fixed metadata set have been durably published. */
    for(unsigned i=0;i<2u && rc==IFS_FS_OK;++i) {
        memcpy(sb,images+l->pos[i],sizeof(sb));sb[209]=0u;
        rc=ifs_fs_host_write_bytes_v2(h,l->at[i],sb,sizeof(sb));
    }
    if(rc==IFS_FS_OK)rc=redo_flush(h);
    for(unsigned i=2u;i<REDO_RANGES && rc==IFS_FS_OK;++i)
        rc=ifs_fs_host_write_bytes_v2(h,l->at[i],next+l->pos[i],l->len[i]);
    if(rc==IFS_FS_OK)rc=redo_flush(h);
    for(int i=1;i>=0 && rc==IFS_FS_OK;--i) {
        rc=ifs_fs_host_write_bytes_v2(h,l->at[i],next+l->pos[i],l->len[i]);
        if(rc==IFS_FS_OK)rc=redo_flush(h);
    }
    if(rc==IFS_FS_OK)rc=ifs_fs_host_write_bytes_v2(h,0u,zero,h->logical_sector_bytes);
    if(rc==IFS_FS_OK)rc=redo_flush(h);
    return rc;
}
int ufs2_relocation_recover(const IfsFsHostRuntimeV2 *h,int apply,
                           uint64_t *repaired,IfsFsVolumeInfoV2 *info)
{
    if(!repaired || !ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    *repaired=0u;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    RedoLayout l;uint8_t marker[REDO_BOOT],digest[SHA256_DIGEST_LENGTH];uint32_t first;
    int rc=redo_header(h,&l,marker,&first);
    if(rc!=IFS_FS_OK)return rc;
    uint8_t *images=h->allocate(h->context,l.bytes);
    if(!images)return IFS_FS_NO_MEMORY;
    rc=ifs_fs_host_read_bytes_v2(h,(uint64_t)first*l.fsize,images,l.bytes);
    if(rc==IFS_FS_OK && (!SHA256(images,l.bytes,digest) ||
                        memcmp(digest,marker+16u,sizeof(digest))))rc=IFS_FS_CORRUPT;
    if(rc==IFS_FS_OK)rc=redo_validate(h,&l,images,first,info);
    if(rc==IFS_FS_OK && apply) {
        rc=redo_apply(h,&l,images);
        if(rc==IFS_FS_OK)*repaired=1u;
    }
    h->release(h->context,images);return rc;
}
int ufs2_relocation_verify(Ufs2Mutation *w,Ufs2RelocationPlan *p)
{
    if(!w || !p || !p->completed)return IFS_FS_INVALID;
    if(w->faulted)return IFS_FS_IO_ERROR;
    int rc=validate_namespace(w);
    if(rc!=IFS_FS_OK)return rc;
    uint8_t *ind=w->host->allocate(w->host->context,w->bsize);
    if(!ind)return IFS_FS_NO_MEMORY;
    for(uint32_t i=0;i<p->count;++i) {
        const Ufs2Placement *e=&p->entries[i];
        const uint8_t *ino=w->inodes+e->inode*INODE_BYTES;
        const uint32_t blocks=blocks_for(w,e->size);
        uint32_t at=e->first;
        if(get64(ino+16u)!=e->size){rc=IFS_FS_CORRUPT;break;}
        if(blocks>12u) {
            rc=rd(w,disk(w,(uint32_t)get64(ino+208u)),ind,w->bsize);
            if(rc!=IFS_FS_OK)break;
        }
        for(uint32_t j=0;j<blocks;++j) {
            uint32_t actual=j<12u?get64(ino+112u+8u*j):get64(ind+8u*(j-12u));
            if(actual!=at){rc=IFS_FS_CORRUPT;break;}
            at+=frags_for(w,e->size,j);
        }
        if(rc!=IFS_FS_OK)break;
        if(at!=e->first+e->fragments ||
           (e->reserve && !placement_free(w,w->cg+CG_FREE,at,e->reserve))) {
            rc=IFS_FS_CORRUPT;break;
        }
    }
    w->host->release(w->host->context,ind);return rc;
}
int ufs2_relocation_execute(Ufs2Mutation *w,Ufs2RelocationPlan *p,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0u;
    if(!w || !p || p->completed)return IFS_FS_INVALID;
    if(w->faulted)return IFS_FS_IO_ERROR;
    if(cancelled(w))return IFS_FS_BUSY;
    if(!p->journal_first){p->completed=1;return ufs2_relocation_verify(w,p);}
    RedoLayout l;int rc=redo_layout(w->host,&l);
    if(rc!=IFS_FS_OK)return rc;
    if(l.fragments!=p->journal_fragments ||
       !placement_free(w,w->cg+CG_FREE,p->journal_first,l.fragments))
        return IFS_FS_CORRUPT;
    const IfsFsHostRuntimeV2 *h=w->host;
    Ufs2Mutation *stage=h->allocate(h->context,sizeof(*stage));
    uint8_t *images=h->allocate(h->context,l.bytes);
    uint8_t *buf=h->allocate(h->context,2u*w->bsize);
    uint8_t marker[REDO_BOOT],summary[16];uint64_t bytes=0u;
    int published=0;
    if(!stage || !images || !buf){rc=IFS_FS_NO_MEMORY;goto done;}
    *stage=*w;
    rc=rd(w,0u,marker,sizeof(marker));
    if(rc!=IFS_FS_OK)goto done;
    if(!redo_zero(marker,sizeof(marker))){rc=IFS_FS_ACCESS;goto done;}
    for(unsigned i=0;i<REDO_RANGES;++i) {
        rc=rd(w,l.at[i],images+l.pos[i],l.len[i]);
        if(rc!=IFS_FS_OK)goto done;
    }
    memcpy(images+l.half,images,l.half);
    uint8_t *ind=buf,*data=buf+w->bsize;
    /* Copy-before-switch. The plan reserved all destinations, growth
     * gaps, indirect blocks and journal against the ORIGINAL bitmap. */
    for(uint32_t i=0;i<p->count;++i) {
        const Ufs2Placement *e=&p->entries[i];
        if(!e->relocate)continue;
        uint8_t *ino=stage->inodes+e->inode*INODE_BYTES;
        const uint32_t blocks=blocks_for(w,e->size);
        const uint32_t oldind=(uint32_t)get64(ino+208u);
        uint32_t at=e->first;
        if(!placement_free(w,w->cg+CG_FREE,at,e->fragments+e->reserve) ||
           get64(ino+16u)!=e->size){rc=IFS_FS_CORRUPT;goto done;}
        if(blocks>12u) {
            if(!oldind || !e->indirect ||
               !placement_free(w,w->cg+CG_FREE,e->indirect,w->frag)) {
                rc=IFS_FS_CORRUPT;goto done;
            }
            rc=rd(w,disk(w,oldind),ind,w->bsize);
            if(rc!=IFS_FS_OK)goto done;
        }
        for(uint32_t j=0;j<blocks;++j) {
            if(cancelled(w)){rc=IFS_FS_BUSY;goto done;}
            uint8_t *ptr=pointer(ino,ind,j);
            const uint32_t old=(uint32_t)get64(ptr),count=frags_for(w,e->size,j);
            if(!valid_run(w,old,count) || !valid_run(w,at,count)) {
                rc=IFS_FS_CORRUPT;goto done;
            }
            rc=rd(w,disk(w,old),data,count*w->fsize);
            if(rc==IFS_FS_OK)rc=wr(w,disk(w,at),data,count*w->fsize);
            if(rc!=IFS_FS_OK)goto done;
            put64(ptr,at);
            for(uint32_t k=0;k<count;++k) {
                setbit(stage->cg+CG_FREE,old+k,1u);
                setbit(stage->cg+CG_FREE,at+k,0u);
            }
            at+=count;
        }
        if(at!=e->first+e->fragments){rc=IFS_FS_CORRUPT;goto done;}
        if(blocks>12u) {
            rc=wr(w,disk(w,e->indirect),ind,w->bsize);
            if(rc!=IFS_FS_OK)goto done;
            put64(ino+208u,e->indirect);
            for(uint32_t k=0;k<w->frag;++k) {
                setbit(stage->cg+CG_FREE,oldind+k,1u);
                setbit(stage->cg+CG_FREE,e->indirect+k,0u);
            }
        }
        bytes+=e->size;
    }
    rc=barrier(w);if(rc!=IFS_FS_OK)goto done;
    prepare_counts(stage,summary);
    stage->sb[209]=1u;
    uint8_t *next=images+l.half;
    memcpy(next+l.pos[0],stage->sb,SB_BYTES);
    memcpy(next+l.pos[1],stage->sb,SB_BYTES);
    put64(next+l.pos[1]+992u,l.at[1]);
    memcpy(next+l.pos[2],stage->cg,stage->cgsize);
    memcpy(next+l.pos[3],stage->inodes,INODES*INODE_BYTES);
    memcpy(next+l.pos[4],summary,sizeof(summary));
    rc=redo_validate(h,&l,images,p->journal_first,NULL);
    if(rc!=IFS_FS_OK)goto done;
    rc=wr(w,disk(w,p->journal_first),images,l.bytes);
    if(rc==IFS_FS_OK)rc=barrier(w);
    if(rc!=IFS_FS_OK)goto done;
    memcpy(marker,redo_magic,sizeof(redo_magic));
    put32(marker+8u,p->journal_first);put32(marker+12u,l.bytes);
    uint8_t marker_hash[SHA256_DIGEST_LENGTH];
    if(!SHA256(images,l.bytes,marker+16u) || !SHA256(marker,48u,marker_hash)){
        rc=IFS_FS_IO_ERROR;goto done;
    }
    memcpy(marker+48u,marker_hash,sizeof(marker_hash));
    published=1; /* even a failed locator write requires a cold check */
    rc=wr(w,0u,marker,h->logical_sector_bytes);
    if(rc==IFS_FS_OK)rc=barrier(w);
    if(rc==IFS_FS_OK)rc=redo_apply(h,&l,images);
    if(rc!=IFS_FS_OK)goto done;
    *w=*stage;p->completed=1;
    rc=ufs2_relocation_verify(w,p);
    if(rc==IFS_FS_OK)*moved=bytes;
done:
    if(rc!=IFS_FS_OK && published)w->faulted=1;
    if(stage)h->release(h->context,stage);
    if(images)h->release(h->context,images);
    if(buf)h->release(h->context,buf);
    return rc;
}
