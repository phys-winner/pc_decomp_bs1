/* Optional launcher cache (<game>/Content/port-textures.tvt: the launcher's texture card, android/texconv/
 * bioshock_tex.cpp, or tools/bioshock_textures.py on a PC); every miss continues through the normal DXT
 * decode. The native Direct3D 10 renderer looks levels up through android/render10/r10_pack.c. For a stored
 * ZIP played in place the game folder is the archive's sidecar folder (files.c maps C:\Games\BioShock there),
 * where the card writes the pack; a separate cache root of the launcher (PORT_TEXTURE_CACHE_ROOT, port.json
 * "textures": "separateCache") holds it as <root>/Content/port-textures.tvt. */
#include "host.h"
#include "../texconv/ut3tx.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t *pack;
static size_t pack_size;
static const utx_entry *entries;
static uint32_t count, family;
static tvtx_crc_tables tables;
_Static_assert(sizeof(tvtx_header)==72,"UTX3 header layout");
_Static_assert(sizeof(utx_entry)==48,"UTX3 entry layout");

int void_texture_cache_family(void) { return pack ? (int)family : 0; }

void void_texture_cache_init(const char *game)
{
    char path[4096];
    const char *root=getenv("PORT_TEXTURE_CACHE_ROOT");
    int separate=root&&*root;
    if(pack||snprintf(path,sizeof(path),"%s/Content/" TVTX_FILE,separate?root:game)>=(int)sizeof(path))return;
    const char *off=getenv("BIOSHOCK_TEXTURE_PACK");   /* QA (perf-flags.env): 0 ignores the pack for A/B runs */
    if(off&&off[0]=='0') { void_log("[textures] BIOSHOCK_TEXTURE_PACK=0: the converted pack is not used");return; }
    char *resolved=separate?NULL:void_path_to_host("C:\\Games\\BioShock\\Content\\" TVTX_FILE);
    int fd=open(resolved?resolved:path,O_RDONLY|O_CLOEXEC);free(resolved);if(fd<0)return;
    struct stat st;tvtx_header h;
    if(fstat(fd,&st)||st.st_size<(off_t)sizeof(h)||pread(fd,&h,sizeof(h),0)!=sizeof(h)) { close(fd);return; }
    uint64_t size=(uint64_t)st.st_size;
    if(h.magic!=TVTX_MAGIC||h.version!=TVTX_VERSION||(h.family!=1&&h.family!=2)||h.count>2000000||
       h.index_offset<sizeof(h)||(h.index_offset&7)||h.index_offset>size||size>SIZE_MAX||
       (uint64_t)h.count*sizeof(utx_entry)!=size-h.index_offset) { close(fd);void_log("[textures] invalid cache header; using DXT");return; }
    const uint8_t *data=mmap(NULL,(size_t)size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    if(data==MAP_FAILED)return;
    /* Levels are read where the game's textures are, not in file order: no readahead around them. */
    madvise((void*)data,(size_t)size,MADV_RANDOM);
    pack_size=(size_t)size;
    const utx_entry *index=(const utx_entry*)(data+h.index_offset);
    for(uint32_t i=0;i<h.count;i++) {
        const utx_entry *e=index+i;
        uint32_t dxt=e->dxt==1?0x31545844u:e->dxt==3?0x33545844u:e->dxt==5?0x35545844u:0;
        uint32_t format=e->gl_format,levels=1,w=e->width,he=e->height;
        while(w>1||he>1) { w=w>1?w/2:1;he=he>1?he/2:1;levels++; }
        uint64_t encoded=0;w=e->width;he=e->height;
        for(unsigned l=0;l<e->levels&&l<16;l++){encoded+=tvtx_level_bytes(format,w,he);w=w>1?w/2:1;he=he>1?he/2:1;}
        if(!dxt||!e->width||!e->height||e->width>16384||e->height>16384||!e->levels||e->levels>levels||
           (h.family==TVTX_ASTC ? format!=TVTX_GL_ASTC_4x4 :
            (format!=TVTX_GL_ETC2_RGB8&&format!=TVTX_GL_ETC2_RGBA8))||
           e->raw_offset<sizeof(h)||e->raw_offset>h.index_offset||e->raw_bytes>h.index_offset-e->raw_offset||
           e->data_offset<sizeof(h)||e->data_offset>h.index_offset||e->data_bytes>h.index_offset-e->data_offset||
           e->raw_bytes!=utx_chain_size(dxt,e->width,e->height,e->levels)||e->data_bytes!=encoded||
           (i&&e->key<=index[i-1].key)) {
            munmap((void*)data,(size_t)size);void_log("[textures] invalid cache index; using DXT");return;
        }
    }
    tvtx_crc_init(&tables);pack=data;entries=index;count=h.count;family=h.family;
    void_log("[textures] optional %s cache: %u mip keys, %llu bytes",h.family==2?"ASTC":"ETC2",count,(unsigned long long)size);
}

const utx_entry *void_texture_cache_find(uint32_t format,uint32_t w,uint32_t h,uint32_t levels,const uint8_t *data,uint32_t pitch)
{
    if(!pack||!data||!w||!h||w>16384||h>16384||!levels||levels>15||
       (format!=0x31545844u&&format!=0x33545844u&&format!=0x35545844u)||
       pitch!=((w+3)/4)*(format==0x31545844u?8:16))return NULL;
    uint64_t bytes=utx_level_size(format,w,h);
    const utx_entry *e=utx_find(entries,count,utx_key(&tables,format,w,h,data,(size_t)bytes));
    unsigned reason=0;
    if(!e)reason=1;
    else if(e->width!=w||e->height!=h||e->dxt!=(format==0x31545844u?1:format==0x33545844u?3:5))reason=2;
    else if(e->levels<levels)reason=3;
    else if(memcmp(data,pack+e->raw_offset,(size_t)bytes))reason=4;
    static uint64_t lookups,results[5];
    ++results[reason];++lookups;
    if(lookups<=8||!(lookups&(lookups-1)))
        void_log("[textures] lookup totals=%llu matches=%llu key_miss=%llu metadata_miss=%llu short_chain=%llu content_miss=%llu last=%ux%u levels=%u indexed_levels=%u",
            (unsigned long long)lookups,(unsigned long long)results[0],(unsigned long long)results[1],
            (unsigned long long)results[2],(unsigned long long)results[3],(unsigned long long)results[4],w,h,levels,e?e->levels:0);
    return reason?NULL:e;
}
const uint8_t *void_texture_cache_raw(const utx_entry *e) { return pack+e->raw_offset; }
const uint8_t *void_texture_cache_data(const utx_entry *e) { return pack+e->data_offset; }

/* Bytes of the pack the caller has read and copied away (a level uploaded, a lookup's comparison): their pages
 * leave the process's mapping (MADV_DONTNEED of a read-only file mapping) and stay in the page cache, from where
 * a later read maps them again.  Mapped, every level the game had loaded stayed resident: with the ASTC pack the
 * Arcadia save kept 327 MiB of the pack in the process (Lenovo TB373FU, work/ha22-perf-20261122). The range
 * goes in whole 64 KiB windows: a fault maps the page cache's pages around it too (the kernel's fault-around,
 * 64 KiB), and with page-sized releases 110 MiB of such neighbours stayed mapped. The mapping is read-only, so a
 * page read again costs only a fault. */
void void_texture_cache_done(const uint8_t *p,size_t bytes)
{
    if(!pack||!p||!bytes||p<pack||p>=pack+pack_size)return;
    const uintptr_t window=65536;
    uintptr_t start=(uintptr_t)p&~(window-1),end=((uintptr_t)p+bytes+window-1)&~(window-1);
    if(start<(uintptr_t)pack)start=(uintptr_t)pack;
    if(end>(uintptr_t)(pack+pack_size))end=(uintptr_t)(pack+pack_size);
    madvise((void*)start,end-start,MADV_DONTNEED);
}
