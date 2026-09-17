#include "pet_dialogue.h"
#include "voice_input.h"
#include "device_api.h"
#include "audio_bus.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define PET_MAX_AUDIO 320000
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static pet_dialogue_state_t state;
static char transcript[1201];
static uint8_t *pcm;
static uint32_t playback_sequence;
static const char *str(cJSON *j,const char *k){cJSON *v=cJSON_GetObjectItem(j,k);return cJSON_IsString(v)?v->valuestring:"";}
pet_dialogue_state_t pet_dialogue_state(void)
{
    taskENTER_CRITICAL(&mux);pet_dialogue_state_t copy=state;taskEXIT_CRITICAL(&mux);
    if(copy.phase==PET_LISTENING&&voice_input_is_processing())copy.phase=PET_THINKING;
    return copy;
}
static void failed(const char *text)
{
    taskENTER_CRITICAL(&mux);state.phase=PET_FAILED;strlcpy(state.reply,text,sizeof(state.reply));taskEXIT_CRITICAL(&mux);
}
static void worker(void *arg)
{
    (void)arg;char id[33],path[96],response[2048];size_t length=0;
    snprintf(id,sizeof(id),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    cJSON *body=cJSON_CreateObject();cJSON_AddStringToObject(body,"requestId",id);cJSON_AddStringToObject(body,"text",transcript);
    char *request=cJSON_PrintUnformatted(body);cJSON_Delete(body);
    esp_err_t result=request?device_api("/v1/pet",request,response,sizeof(response),&length):ESP_ERR_NO_MEM;
    cJSON_free(request);memset(transcript,0,sizeof(transcript));
    snprintf(path,sizeof(path),"/v1/pet?id=%s",id);
    pet_dialogue_state_t next={.phase=PET_READY};char hash[65]={0};bool ready=false;
    /* No automatic resubmission of a paid model request after a timeout. */
    int64_t deadline=esp_timer_get_time()+180000000;
    for(unsigned attempt=0;result==ESP_OK&&attempt<100&&esp_timer_get_time()<deadline;attempt++) {
        cJSON *j=cJSON_Parse(response);
        if(!j){result=ESP_FAIL;break;}
        const char *status=str(j,"status");
        if(!strcmp(status,"done")) {
            const char *reply=str(j,"reply"),*digest=str(j,"sha256"),*action=str(j,"action");
            cJSON *bytes=cJSON_GetObjectItem(j,"bytes");
            ready=*reply&&strlen(reply)<sizeof(next.reply)&&strlen(digest)==64&&strspn(digest,"0123456789abcdef")==64&&cJSON_IsNumber(bytes)&&bytes->valuedouble==bytes->valueint&&bytes->valueint>=3200&&bytes->valueint<=PET_MAX_AUDIO&&!(bytes->valueint%2);
            if(ready){strlcpy(next.reply,reply,sizeof(next.reply));strlcpy(hash,digest,sizeof(hash));next.bytes=bytes->valueint;
                next.action=!strcmp(action,"pat")?1:!strcmp(action,"feed")?2:!strcmp(action,"sleep")?3:!strcmp(action,"wake")?4:0;}
            cJSON_Delete(j);break;
        }
        bool working=!strcmp(status,"working");cJSON_Delete(j);
        if(!working){result=ESP_FAIL;break;}
        vTaskDelay(pdMS_TO_TICKS(2000));
        result=device_api(path,NULL,response,sizeof(response),&length);
    }
    if(ready) {
        if(!pcm)pcm=heap_caps_malloc(PET_MAX_AUDIO+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        snprintf(path,sizeof(path),"/v1/pet?id=%s&audio=1",id);
        result=pcm?device_api(path,NULL,pcm,PET_MAX_AUDIO+1,&length):ESP_ERR_NO_MEM;
        uint8_t digest[32];char hex[65];
        if(result==ESP_OK&&length==next.bytes){mbedtls_sha256(pcm,length,digest,0);for(int i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);ready=!strcmp(hex,hash);}
        else ready=false;
    }
    if(ready){taskENTER_CRITICAL(&mux);next.sequence=state.sequence+1;state=next;taskEXIT_CRITICAL(&mux);}
    else failed("团团暂时没能回答，请稍后再试");
    vTaskDelete(NULL);
}
static void recognized(const char *text,void *context)
{
    (void)context;
    if(!text||!*text||strlen(text)>=sizeof(transcript)){failed("没有听清，请靠近一点再说");return;}
    strlcpy(transcript,text,sizeof(transcript));
    taskENTER_CRITICAL(&mux);state.phase=PET_THINKING;taskEXIT_CRITICAL(&mux);
    if(xTaskCreateWithCaps(worker,"pet_reply",8192,NULL,3,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS)failed("内存正忙，请稍后再试");
}
static bool reserve(void)
{
    taskENTER_CRITICAL(&mux);
    bool available=state.phase!=PET_LISTENING&&state.phase!=PET_THINKING;
    if(available){state.phase=PET_LISTENING;state.reply[0]=0;}
    taskEXIT_CRITICAL(&mux);return available;
}
esp_err_t pet_dialogue_start(void)
{
    if(!reserve())return ESP_ERR_INVALID_STATE;
    esp_err_t result=voice_input_start_with_callback(recognized,NULL);
    if(result!=ESP_OK)failed("声音正忙，请稍后再说");
    return result;
}
void pet_dialogue_finish(void)
{
    /* Publish the transition before waking the capture task; its short-capture
     * failure callback must never be overwritten with a permanent THINKING. */
    taskENTER_CRITICAL(&mux);bool listening=state.phase==PET_LISTENING;
    if(listening)state.phase=PET_THINKING;
    taskEXIT_CRITICAL(&mux);
    if(listening)voice_input_finish();
}
esp_err_t pet_dialogue_play(void)
{
    pet_dialogue_state_t s=pet_dialogue_state();
    if(s.phase!=PET_READY||!pcm)return ESP_ERR_INVALID_STATE;
    esp_err_t result=audio_local_play(pcm,s.bytes);
    if(result==ESP_OK)playback_sequence=audio_local_state().sequence;
    return result;
}
void pet_dialogue_leave(void)
{
    pet_dialogue_finish();
    audio_local_stop(playback_sequence);
}
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
esp_err_t pet_dialogue_test_text(const char *text)
{
    if(!reserve())return ESP_ERR_INVALID_STATE;
    recognized(text,NULL);return ESP_OK;
}
#endif
