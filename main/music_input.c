#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "music_input.h"
#include "notification_center.h"
#include "voice_input.h"
#include "audio_bus.h"
#include "service_config.h"


#define MUSIC_PROMPT_BYTES 1024
#define MUSIC_RESPONSE_BYTES 1024
#define MUSIC_STREAM_BYTES 2048
#define MUSIC_MAX_PCM_BYTES (12 * 1024 * 1024)
#define MUSIC_SAMPLE_RATE 16000

extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");

typedef enum {
    MUSIC_IDLE,
    MUSIC_RECORDING,
    MUSIC_CREATING,
    MUSIC_READY,
    MUSIC_PLAYING,
} music_state_t;

static const char *TAG = "music_input";
static volatile music_state_t music_state = MUSIC_IDLE;
static char music_status[MUSIC_RESPONSE_BYTES] = "READY FOR MUSIC IDEA";
static char music_prompt[MUSIC_PROMPT_BYTES];
static char music_prefix[MUSIC_PROMPT_BYTES];
static TaskHandle_t music_task_handle;
static char playback_url[180]=CLOCK_MUSIC_AUDIO_URL;
static volatile bool stop_playback;

static void set_music_status(const char *status)
{
    if (status != NULL) {
        strlcpy(music_status, status, sizeof(music_status));
        if(!strcmp(status,"CREATING MUSIC...")) notification_post("music", "音乐正在生成，可继续使用其他应用",music_prefix[0]?11:10,NOTICE_WORKING);
        else if(!strcmp(status,"MUSIC PLAYBACK FAILED")) notification_post("music","音乐播放失败，点此查看并重试",10,NOTICE_FAILED);
        else if(strstr(status,"FAILED")) notification_post("music","音乐处理失败，点此查看并重试",music_prefix[0]?11:10,NOTICE_FAILED);
        ESP_LOGI(TAG, "%s", music_status);
    }
}

static void set_auth(esp_http_client_handle_t client)
{
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }
}

static esp_err_t create_music(const char *prompt)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || !cJSON_AddStringToObject(root, "prompt", prompt)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t config = {
        .url = CLOCK_MUSIC_URL,
        .timeout_ms = 180000,
        .buffer_size = 2048,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    set_auth(client);
    esp_err_t result = esp_http_client_open(client, (int)strlen(body));
    size_t sent = 0, body_length = strlen(body);
    while (result == ESP_OK && sent < body_length) {
        int written = esp_http_client_write(client, body + sent, body_length - sent);
        if (written <= 0) result = ESP_FAIL;
        else sent += (size_t)written;
    }
    cJSON_free(body);
    char *response = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) result = ESP_ERR_NO_MEM;
    if (result == ESP_OK) {
        int64_t length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200 || length >= (int64_t)4096) {
            ESP_LOGE(TAG, "Music creation HTTP status=%d length=%lld", status, length);
            result = ESP_FAIL;
        } else {
            int received = esp_http_client_read_response(client, response, 4095);
            if (received <= 0) {
                result = ESP_ERR_INVALID_RESPONSE;
            } else {
                response[received] = '\0';
                cJSON *result_json = cJSON_Parse(response);
                cJSON *ready = result_json == NULL ? NULL :
                    cJSON_GetObjectItemCaseSensitive(result_json, "ready");
                if (!cJSON_IsTrue(ready)) {
                    result = ESP_ERR_INVALID_RESPONSE;
                }
                cJSON_Delete(result_json);
            }
        }
    }
    heap_caps_free(response);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t play_latest_music(void)
{
    esp_err_t initialized = audio_bus_open_speaker();
    if (initialized != ESP_OK) return initialized;
    esp_http_client_config_t config = {
        .url = playback_url,
        .timeout_ms = 10000,
        .buffer_size = MUSIC_STREAM_BYTES,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    set_auth(client);
    esp_err_t result = esp_http_client_open(client, 0);
    if (result == ESP_OK) {
        int64_t length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200 || length <= 0 || length > MUSIC_MAX_PCM_BYTES || length % 2) {
            ESP_LOGE(TAG, "Music stream HTTP status=%d length=%lld", status, length);
            result = ESP_FAIL;
        } else {
            uint8_t buffer[MUSIC_STREAM_BYTES];
            int received, carry = 0;
            received=0;
            while (!stop_playback && (received = esp_http_client_read(client, (char *)buffer + carry, sizeof(buffer) - carry)) > 0) {
                int total = received + carry;
                int aligned = total & ~1;
                if (aligned && audio_bus_write(buffer, aligned) != ESP_OK) {
                    result = ESP_FAIL;
                    break;
                }
                carry = total & 1;
                if (carry) buffer[0] = buffer[aligned];
            }
            if (!stop_playback && (received < 0 || carry || !esp_http_client_is_complete_data_received(client))) {
                result = ESP_FAIL;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static void music_task(void *context)
{
    bool playback = (bool)(intptr_t)context;
    esp_err_t result = playback ? play_latest_music() : create_music(music_prompt);
    if (playback) audio_bus_release();
    if (result == ESP_OK) {
        music_state = MUSIC_READY;
        if(!playback) notification_post("music","音乐已生成，点此播放",music_prefix[0]?11:10,NOTICE_DONE);
        set_music_status(playback ? "MUSIC READY - TAP PLAY" : "MUSIC READY - TAP PLAY");
    } else {
        music_state = MUSIC_IDLE;
        set_music_status(playback ? "MUSIC PLAYBACK FAILED" : "MUSIC CREATION FAILED");
    }
    music_task_handle = NULL;
    vTaskDelete(NULL);
}

static void music_transcript_ready(const char *text, void *context)
{
    (void)context;
    if (text == NULL || text[0] == '\0' || music_task_handle != NULL) {
        music_state = MUSIC_IDLE;
        set_music_status("MUSIC IDEA FAILED");
        return;
    }
    if (music_prefix[0] != '\0') {
        strlcpy(music_prompt, music_prefix, sizeof(music_prompt));
        strlcat(music_prompt, " Spoken listener idea: ", sizeof(music_prompt));
        strlcat(music_prompt, text, sizeof(music_prompt));
    } else {
        strlcpy(music_prompt, text, sizeof(music_prompt));
    }
    music_state = MUSIC_CREATING;
    set_music_status("CREATING MUSIC...");
    if (xTaskCreate(music_task, "music_create", 8192, NULL, 4, &music_task_handle) != pdPASS) {
        music_task_handle = NULL;
        music_state = MUSIC_IDLE;
        set_music_status("MUSIC CREATION FAILED");
    }
}

esp_err_t music_input_start(void)
{
    return music_input_start_with_prefix(NULL);
}

esp_err_t music_input_start_with_prefix(const char *prefix)
{
    if ((music_state != MUSIC_IDLE && music_state != MUSIC_READY) ||
        voice_input_is_recording()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (prefix == NULL) {
        music_prefix[0] = '\0';
    } else {
        strlcpy(music_prefix, prefix, sizeof(music_prefix));
    }
    esp_err_t result = voice_input_start_with_callback(music_transcript_ready, NULL);
    if (result == ESP_OK) {
        music_state = MUSIC_RECORDING;
        set_music_status("RECORDING MUSIC IDEA - TAP STOP");
    }
    return result;
}

esp_err_t music_input_finish(void)
{
    if (music_state != MUSIC_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = voice_input_finish();
    if (result == ESP_OK) {
        set_music_status("RECOGNIZING MUSIC IDEA...");
    }
    return result;
}

static esp_err_t play_url(const char *url)
{
    if ((music_state != MUSIC_READY && music_state != MUSIC_IDLE) ||
        music_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_bus_try_acquire()) return ESP_ERR_INVALID_STATE;
    strlcpy(playback_url,url,sizeof(playback_url));stop_playback=false;
    music_state = MUSIC_PLAYING;
    set_music_status("PLAYING MUSIC...");
    if (xTaskCreate(music_task, "music_play", 8192, (void *)(intptr_t)true, 4,
                    &music_task_handle) != pdPASS) {
        audio_bus_release();
        music_task_handle = NULL;
        music_state = MUSIC_READY;
        set_music_status("MUSIC PLAYBACK FAILED");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t music_input_play(void) { return play_url(CLOCK_MUSIC_AUDIO_URL); }
esp_err_t music_input_play_artwork(const char *id)
{
    if(!id || strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32) return ESP_ERR_INVALID_ARG;
    char url[256];snprintf(url,sizeof(url),CLOCK_API_BASE "/v1/library?id=%s&part=data",id);
    return play_url(url);
}
void music_input_stop(void) { stop_playback=true; }

void music_input_set_volume(uint8_t volume)
{
    audio_bus_set_volume(volume);
}

bool music_input_is_recording(void)
{
    return music_state == MUSIC_RECORDING;
}

const char *music_input_status(void)
{
    return music_status;
}
