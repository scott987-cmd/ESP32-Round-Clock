#include "audio_bus.h"
#include "content_assets.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static audio_focus_t focus;
static SemaphoreHandle_t codec_mutex;
static uint8_t volume=70;
static esp_codec_dev_handle_t speaker;
static audio_local_state_t local;
static audio_lease_t local_lease;
typedef struct {
    uint8_t *pcm;
    size_t bytes;
    uint32_t offset,sequence;
    unsigned kind;
    audio_lease_t lease;
    char path[96];
} local_job_t;

void audio_bus_init(void)
{
    codec_mutex=xSemaphoreCreateMutex();
    configASSERT(codec_mutex);
}
audio_lease_t audio_bus_request(void)
{
    taskENTER_CRITICAL(&mux);audio_lease_t lease=audio_focus_request(&focus);taskEXIT_CRITICAL(&mux);
    return lease;
}
bool audio_bus_is_current(audio_lease_t lease)
{
    taskENTER_CRITICAL(&mux);bool current=audio_focus_current(&focus,lease);taskEXIT_CRITICAL(&mux);
    return current;
}
void audio_bus_release(audio_lease_t lease)
{
    taskENTER_CRITICAL(&mux);audio_focus_release(&focus,lease);taskEXIT_CRITICAL(&mux);
}
bool audio_bus_finish(audio_lease_t lease)
{
    taskENTER_CRITICAL(&mux);
    bool current=audio_focus_current(&focus,lease);
    audio_focus_release(&focus,lease);
    taskEXIT_CRITICAL(&mux);return current;
}
esp_err_t audio_bus_lock(audio_lease_t lease)
{
    if(!audio_bus_is_current(lease))return ESP_ERR_INVALID_STATE;
    if(!codec_mutex||xSemaphoreTake(codec_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    if(!audio_bus_is_current(lease)){xSemaphoreGive(codec_mutex);return ESP_ERR_INVALID_STATE;}
    return ESP_OK;
}
void audio_bus_unlock(void) {xSemaphoreGive(codec_mutex);}
void audio_bus_set_volume(uint8_t value)
{
    taskENTER_CRITICAL(&mux); volume=value>100?100:value; taskEXIT_CRITICAL(&mux);
}
uint8_t audio_bus_volume(void)
{
    taskENTER_CRITICAL(&mux); uint8_t value=volume; taskEXIT_CRITICAL(&mux); return value;
}
static esp_err_t open_speaker(void)
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
esp_err_t audio_bus_write(audio_lease_t lease,void *pcm,size_t bytes)
{
    if(!pcm || !bytes || bytes%2) return ESP_ERR_INVALID_ARG;
    esp_err_t result=audio_bus_lock(lease);
    if(result!=ESP_OK)return result;
    result=open_speaker();
    if(result==ESP_OK&&esp_codec_dev_write(speaker,pcm,bytes)!=ESP_CODEC_DEV_OK)result=ESP_FAIL;
    audio_bus_unlock();return result;
}
audio_local_state_t audio_local_state(void)
{
    taskENTER_CRITICAL(&mux); audio_local_state_t value=local; taskEXIT_CRITICAL(&mux); return value;
}
void audio_local_stop(uint32_t sequence)
{
    taskENTER_CRITICAL(&mux);
    if(sequence&&local.sequence==sequence)audio_focus_release(&focus,local_lease);
    taskEXIT_CRITICAL(&mux);
}
static void local_task(void *context)
{
    local_job_t *job=context;
    esp_err_t result=ESP_OK;
    FILE *file=job->kind==2?fopen(job->path,"rb"):NULL;
    if(job->kind==2&&!file)result=ESP_FAIL;
    uint8_t buffer[1024];
    size_t written=0;
    while(result==ESP_OK && written<job->bytes) {
        if(!audio_bus_is_current(job->lease)) { result=ESP_ERR_INVALID_STATE; break; }
        size_t count=job->bytes-written;
        if(count>sizeof(buffer)) count=sizeof(buffer);
        if(job->kind==1)result=content_read(job->offset+written,buffer,count);
        else if(job->kind==2) {if(fread(buffer,1,count,file)!=count)result=ESP_FAIL;}
        else memcpy(buffer,job->pcm+written,count);
        if(result==ESP_OK)result=audio_bus_write(job->lease,buffer,count);
        if(result==ESP_OK) written+=count;
    }
    if(file)fclose(file);
    taskENTER_CRITICAL(&mux);
    if(local.sequence==job->sequence){local.result=result;local.written=written;local.playing=false;}
    audio_focus_release(&focus,job->lease);
    taskEXIT_CRITICAL(&mux);
    heap_caps_free(job->pcm);
    free(job);
    vTaskDelete(NULL);
}
static esp_err_t play_source(unsigned kind,const uint8_t *pcm,uint32_t offset,const char *path,size_t bytes)
{
    if((kind==0&&!pcm) || bytes<2 || bytes%2 || bytes>4*1024*1024) return ESP_ERR_INVALID_ARG;
    if(kind==1&&!content_assets_ready())return ESP_ERR_INVALID_STATE;
    if(kind==2&&(!path || strncmp(path,"/wallpaper/story-",17) || strlen(path)>=sizeof(((local_job_t *)0)->path)))return ESP_ERR_INVALID_ARG;
    local_job_t *job=calloc(1,sizeof(*job));if(!job)return ESP_ERR_NO_MEM;
    /* The caller may replace its response after a new recording takes focus. */
    if(kind==0){job->pcm=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!job->pcm){free(job);return ESP_ERR_NO_MEM;}memcpy(job->pcm,pcm,bytes);}
    job->kind=kind;job->bytes=bytes;job->offset=offset;
    if(path)strlcpy(job->path,path,sizeof(job->path));
    taskENTER_CRITICAL(&mux);
    job->lease=local_lease=audio_focus_request(&focus);
    if(++local.sequence==0)++local.sequence;
    local.playing=true; local.result=ESP_ERR_NOT_FINISHED; local.written=0;
    job->sequence=local.sequence;
    taskEXIT_CRITICAL(&mux);
    if(xTaskCreate(local_task,"word_audio",4096,job,4,NULL)!=pdPASS) {
        taskENTER_CRITICAL(&mux);
        if(local.sequence==job->sequence){local.playing=false;local.result=ESP_ERR_NO_MEM;}
        audio_focus_release(&focus,job->lease);
        taskEXIT_CRITICAL(&mux);
        heap_caps_free(job->pcm);free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t audio_local_play(const uint8_t *pcm,size_t bytes) {return play_source(0,pcm,0,NULL,bytes);}
esp_err_t audio_local_play_asset(uint32_t offset,size_t bytes) {return play_source(1,NULL,offset,NULL,bytes);}
esp_err_t audio_local_play_file(const char *path,size_t bytes) {return play_source(2,NULL,0,path,bytes);}
