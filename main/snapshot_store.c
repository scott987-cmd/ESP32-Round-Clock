#include "snapshot_store.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define SNAPSHOT_LIMIT (64*1024)
#define MAGIC 0x52435331u
typedef struct {uint32_t magic,sequence,size,captured_at,checksum;} header_t;
static uint32_t hash(uint32_t h,const void *data,size_t n)
{
    const unsigned char *p=data;while(n--)h=(h^*p++)*16777619u;return h;
}
static uint32_t header_hash(const header_t *h){return hash(2166136261u,h,offsetof(header_t,checksum));}
static bool path_for(char *path,size_t n,const char *base,unsigned slot)
{
    return base&&snprintf(path,n,"%s.%u",base,slot)<(int)n;
}
static bool inspect(const char *base,unsigned slot,header_t *h)
{
    char path[112];if(!path_for(path,sizeof(path),base,slot))return false;
    FILE *f=fopen(path,"rb");if(!f)return false;
    bool ok=fread(h,1,sizeof(*h),f)==sizeof(*h)&&h->magic==MAGIC&&h->sequence&&h->size&&h->size<=SNAPSHOT_LIMIT;
    uint32_t sum=ok?header_hash(h):0;unsigned char block[256];size_t left=ok?h->size:0;
    while(ok&&left){size_t n=left<sizeof(block)?left:sizeof(block);if(fread(block,1,n,f)!=n){ok=false;break;}sum=hash(sum,block,n);left-=n;}
    ok=ok&&sum==h->checksum&&fgetc(f)==EOF&&!ferror(f);fclose(f);return ok;
}
static int newest(const char *base,header_t h[2])
{
    bool a=inspect(base,0,&h[0]),b=inspect(base,1,&h[1]);
    if(!a&&!b)return -1;
    return !a?1:!b?0:(int32_t)(h[1].sequence-h[0].sequence)>0?1:0;
}
size_t snapshot_load(const char *base,void *data,size_t capacity,snapshot_meta_t *meta)
{
    if(!data)return 0;
    header_t h[2];int slot=newest(base,h);
    if(slot<0||h[slot].size>capacity)return 0;
    char path[112];if(!path_for(path,sizeof(path),base,(unsigned)slot))return 0;
    FILE *f=fopen(path,"rb");if(!f)return 0;
    bool ok=fseek(f,sizeof(header_t),SEEK_SET)==0&&fread(data,1,h[slot].size,f)==h[slot].size;
    fclose(f);
    if(!ok||hash(header_hash(&h[slot]),data,h[slot].size)!=h[slot].checksum)return 0;
    if(meta)*meta=(snapshot_meta_t){h[slot].sequence,h[slot].captured_at};
    return h[slot].size;
}
bool snapshot_save(const char *base,const void *data,size_t size,uint32_t captured_at)
{
    if(!data||!size||size>SNAPSHOT_LIMIT)return false;
    header_t old[2];int slot=newest(base,old);
    header_t h={.magic=MAGIC,.sequence=slot<0?1:old[slot].sequence+1,.size=(uint32_t)size,.captured_at=captured_at};
    if(!h.sequence)h.sequence=1;
    /* Do not wear flash just to refresh a timestamp when content is unchanged. */
    if(slot>=0&&old[slot].size==size&&hash(header_hash(&old[slot]),data,size)==old[slot].checksum)return true;
    h.checksum=hash(header_hash(&h),data,size);
    unsigned target=slot==0?1:0;char path[112];if(!path_for(path,sizeof(path),base,target))return false;
    FILE *f=fopen(path,"wb");if(!f)return false;
    bool ok=fwrite(&h,1,sizeof(h),f)==sizeof(h)&&fwrite(data,1,size,f)==size;
    if(fflush(f))ok=false;
    if(fclose(f))ok=false;
    header_t checked;return ok&&inspect(base,target,&checked);
}
