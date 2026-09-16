#include "audio_bus.h"
#include "content_assets.h"
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static bool reserved, stop_requested;
static uint8_t volume=70;
static esp_codec_dev_handle_t speaker;
static audio_local_state_t local;
static const uint8_t *local_pcm;
static size_t local_bytes;
static uint32_t asset_offset;
static unsigned source_kind;
static char audio_path[96];

bool audio_bus_try_acquire(void)
{
    taskENTER_CRITICAL(&mux);
    bool acquired=!reserved;
    if(acquired) reserved=true;
    taskEXIT_CRITICAL(&mux);
    return acquired;
}
void audio_bus_release(void)
{
    taskENTER_CRITICAL(&mux); reserved=false; taskEXIT_CRITICAL(&mux);
}
void audio_bus_set_volume(uint8_t value)
{
    taskENTER_CRITICAL(&mux); volume=value>100?100:value; taskEXIT_CRITICAL(&mux);
}
uint8_t audio_bus_volume(void)
{
    taskENTER_CRITICAL(&mux); uint8_t value=volume; taskEXIT_CRITICAL(&mux); return value;
}
esp_err_t audio_bus_open_speaker(void)
{
    if(!speaker) {
        speaker=bsp_audio_codec_speaker_init();
        if(!speaker) return ESP_FAIL;
        esp_codec_dev_sample_info_t sample={.bits_per_sample=16,.channel=1,.sample_rate=16000};
        if(esp_codec_dev_open(speaker,&sample)!=ESP_CODEC_DEV_OK) {
            esp_codec_dev_delete(speaker); speaker=NULL; return ESP_FAIL;
        }
    }
    return esp_codec_dev_set_out_vol(speaker,audio_bus_volume())==ESP_CODEC_DEV_OK?ESP_OK:ESP_FAIL;
}
esp_err_t audio_bus_write(void *pcm,size_t bytes)
{
    if(!speaker || !pcm || !bytes || bytes%2) return ESP_ERR_INVALID_ARG;
    if(esp_codec_dev_set_out_vol(speaker,audio_bus_volume())!=ESP_CODEC_DEV_OK) return ESP_FAIL;
    return esp_codec_dev_write(speaker,pcm,bytes)==ESP_CODEC_DEV_OK?ESP_OK:ESP_FAIL;
}
audio_local_state_t audio_local_state(void)
{
    taskENTER_CRITICAL(&mux); audio_local_state_t value=local; taskEXIT_CRITICAL(&mux); return value;
}
void audio_local_stop(void)
{
    taskENTER_CRITICAL(&mux); if(local.playing) stop_requested=true; taskEXIT_CRITICAL(&mux);
}
static void local_task(void *unused)
{
    (void)unused;
    esp_err_t result=audio_bus_open_speaker();
    FILE *file=source_kind==2?fopen(audio_path,"rb"):NULL;
    if(source_kind==2&&!file)result=ESP_FAIL;
    uint8_t buffer[1024];
    size_t written=0;
    while(result==ESP_OK && written<local_bytes) {
        taskENTER_CRITICAL(&mux); bool stopped=stop_requested; taskEXIT_CRITICAL(&mux);
        if(stopped) { result=ESP_ERR_INVALID_STATE; break; }
        size_t count=local_bytes-written;
        if(count>sizeof(buffer)) count=sizeof(buffer);
        if(source_kind==1)result=content_read(asset_offset+written,buffer,count);
        else if(source_kind==2) {if(fread(buffer,1,count,file)!=count)result=ESP_FAIL;}
        else memcpy(buffer,local_pcm+written,count);
        if(result==ESP_OK)result=audio_bus_write(buffer,count);
        if(result==ESP_OK) written+=count;
    }
    if(file)fclose(file);
    taskENTER_CRITICAL(&mux);
    local.result=result; local.written=written; local.playing=false; reserved=false;
    taskEXIT_CRITICAL(&mux);
    vTaskDelete(NULL);
}
static esp_err_t play_source(unsigned kind,const uint8_t *pcm,uint32_t offset,const char *path,size_t bytes)
{
    if((kind==0&&!pcm) || bytes<2 || bytes%2 || bytes>4*1024*1024) return ESP_ERR_INVALID_ARG;
    if(kind==1&&!content_assets_ready())return ESP_ERR_INVALID_STATE;
    if(kind==2&&(!path || strncmp(path,"/wallpaper/story-",17) || strlen(path)>=sizeof(audio_path)))return ESP_ERR_INVALID_ARG;
    if(!audio_bus_try_acquire()) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&mux);
    local_pcm=pcm; local_bytes=bytes; stop_requested=false;
    source_kind=kind;asset_offset=offset;if(path)strlcpy(audio_path,path,sizeof(audio_path));
    local.sequence++; local.playing=true; local.result=ESP_ERR_NOT_FINISHED; local.written=0;
    taskEXIT_CRITICAL(&mux);
    if(xTaskCreate(local_task,"word_audio",4096,NULL,4,NULL)!=pdPASS) {
        taskENTER_CRITICAL(&mux);
        local.playing=false; local.result=ESP_ERR_NO_MEM; reserved=false;
        taskEXIT_CRITICAL(&mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t audio_local_play(const uint8_t *pcm,size_t bytes) {return play_source(0,pcm,0,NULL,bytes);}
esp_err_t audio_local_play_asset(uint32_t offset,size_t bytes) {return play_source(1,NULL,offset,NULL,bytes);}
esp_err_t audio_local_play_file(const char *path,size_t bytes) {return play_source(2,NULL,0,path,bytes);}
