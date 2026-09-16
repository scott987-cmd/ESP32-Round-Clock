#include "content_assets.h"
#include "content_index.h"
#include "english_assets.h"
#include "audio_bus.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "zlib.h"
#include <string.h>
static const esp_partition_t *partition;
static bool valid;
esp_err_t content_assets_init(void)
{
    partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"content");
    if(!partition || partition->size<CONTENT_BYTES)return ESP_ERR_NOT_FOUND;
    uint32_t header[4];if(esp_partition_read(partition,0,header,16)!=ESP_OK)return ESP_FAIL;
    if(memcmp(header,"RCAS",4)||header[1]!=1||header[2]!=CONTENT_BYTES-16||header[3]!=CONTENT_CRC)return ESP_ERR_INVALID_CRC;
    uint8_t *buffer=heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!buffer)return ESP_ERR_NO_MEM;
    uint32_t crc=0;esp_err_t result=ESP_OK;
    for(size_t offset=16;offset<CONTENT_BYTES;offset+=4096) {
        size_t n=CONTENT_BYTES-offset;if(n>4096)n=4096;
        result=esp_partition_read(partition,offset,buffer,n);if(result!=ESP_OK)break;
        crc=esp_rom_crc32_le(crc,buffer,n);
    }
    heap_caps_free(buffer);valid=result==ESP_OK&&crc==CONTENT_CRC;
    return valid?ESP_OK:ESP_ERR_INVALID_CRC;
}
bool content_assets_ready(void) {return valid;}
esp_err_t content_read(uint32_t offset,void *target,size_t bytes)
{
    if(!valid || !target || offset>CONTENT_BYTES || bytes>CONTENT_BYTES-offset)return ESP_ERR_INVALID_ARG;
    return esp_partition_read(partition,offset,target,bytes);
}
static voidpf allocate(voidpf p,uInt n,uInt size){(void)p;return heap_caps_calloc(n,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static void release(voidpf p,voidpf data){(void)p;heap_caps_free(data);}
bool content_picture(unsigned word,unsigned size,uint8_t *target)
{
    if(!target || word>=ENGLISH_WORD_COUNT || (size!=128&&size!=176))return false;
    const english_asset_t *entry=&english_assets[word];uint32_t offset=size==128?entry->thumbnail:entry->image,bytes=size==128?entry->thumbnail_bytes:entry->image_bytes;
    uint8_t *compressed=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!compressed)return false;
    bool ok=false;
    if(content_read(CONTENT_ENGLISH_OFFSET+offset,compressed,bytes)==ESP_OK) {
        z_stream s={.zalloc=allocate,.zfree=release,.next_in=compressed,.avail_in=bytes,.next_out=target,.avail_out=size*size*2};
        if(inflateInit(&s)==Z_OK) {ok=inflate(&s,Z_FINISH)==Z_STREAM_END&&s.total_out==size*size*2&&s.avail_in==0;inflateEnd(&s);}
    }
    heap_caps_free(compressed);return ok;
}
esp_err_t content_word_play(unsigned word)
{
    if(word>=ENGLISH_WORD_COUNT)return ESP_ERR_INVALID_ARG;
    return audio_local_play_asset(CONTENT_ENGLISH_OFFSET+english_assets[word].audio,english_assets[word].audio_bytes);
}
