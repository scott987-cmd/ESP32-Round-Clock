#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "voice_input.h"
#include "wallpaper_input.h"
#include "notification_center.h"
#include "service_config.h"

#define WALLPAPER_PROMPT_BYTES 1536

extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");

typedef enum { WALLPAPER_IDLE, WALLPAPER_RECORDING, WALLPAPER_GENERATING } wallpaper_state_t;

static const char *TAG = "wallpaper_input";
static volatile wallpaper_state_t wallpaper_state = WALLPAPER_IDLE;
static volatile bool reload_requested;
static volatile bool generated_update_pending;
static TaskHandle_t wallpaper_task_handle;
static char wallpaper_status[128] = "READY FOR WALLPAPER IDEA";
static char wallpaper_prompt[WALLPAPER_PROMPT_BYTES];

static void set_wallpaper_status(const char *status)
{
    strlcpy(wallpaper_status, status, sizeof(wallpaper_status));
    if(!strcmp(status,"CREATING WALLPAPER...")) notification_post("wallpaper","壁纸正在生成，可继续使用其他应用",12,NOTICE_WORKING);
    else if(!strcmp(status,"WALLPAPER SAVED ON DEVICE")) notification_post("wallpaper","新壁纸已保存到设备，点此查看",1,NOTICE_DONE);
    else if(strstr(status,"FAILED")) notification_post("wallpaper","壁纸处理失败，点此查看并重试",12,NOTICE_FAILED);
    ESP_LOGI(TAG, "%s", wallpaper_status);
}

static void set_auth(esp_http_client_handle_t client)
{
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char authorization[192];
        snprintf(authorization, sizeof(authorization), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
}

static esp_err_t create_wallpaper(const char *prompt)
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
        .url = CLOCK_WALLPAPER_GENERATE_URL,
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
    esp_err_t result = esp_http_client_open(client, strlen(body));
    size_t sent = 0, body_length = strlen(body);
    while (result == ESP_OK && sent < body_length) {
        int written = esp_http_client_write(client, body + sent, body_length - sent);
        if (written <= 0) result = ESP_FAIL;
        else sent += (size_t)written;
    }
    cJSON_free(body);
    /* Metadata includes the prompt. Keep this buffer off the 8 KiB task stack. */
    char *response = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) result = ESP_ERR_NO_MEM;
    if (result == ESP_OK) {
        int64_t length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int received = length >= (int64_t)4096 ? -1 :
                       esp_http_client_read_response(client, response, 4095);
        cJSON *json = received > 0 ? cJSON_Parse(response) : NULL;
        cJSON *ready = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "ready");
        if (status != 200 || !cJSON_IsTrue(ready)) {
            ESP_LOGE(TAG, "Wallpaper HTTP=%d length=%lld received=%d", status, length, received);
            result = ESP_FAIL;
        }
        cJSON_Delete(json);
    }
    heap_caps_free(response);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static void wallpaper_generate_task(void *context)
{
    (void)context;
    if (create_wallpaper(wallpaper_prompt) == ESP_OK) {
        generated_update_pending = true;
        reload_requested = true;
        set_wallpaper_status("WALLPAPER READY - UPDATING");
    } else {
        set_wallpaper_status("WALLPAPER CREATION FAILED");
    }
    wallpaper_state = WALLPAPER_IDLE;
    wallpaper_task_handle = NULL;
    vTaskDelete(NULL);
}

static void wallpaper_transcript_ready(const char *text, void *context)
{
    (void)context;
    if (text == NULL || text[0] == '\0') {
        wallpaper_state = WALLPAPER_IDLE;
        set_wallpaper_status("WALLPAPER IDEA FAILED");
        return;
    }
    strlcpy(wallpaper_prompt, text, sizeof(wallpaper_prompt));
    wallpaper_state = WALLPAPER_GENERATING;
    set_wallpaper_status("CREATING WALLPAPER...");
    if (xTaskCreate(wallpaper_generate_task, "wallpaper_generate", 8192, NULL, 4,
                    &wallpaper_task_handle) != pdPASS) {
        wallpaper_task_handle = NULL;
        wallpaper_state = WALLPAPER_IDLE;
        set_wallpaper_status("WALLPAPER CREATION FAILED");
    }
}

esp_err_t wallpaper_input_start(void)
{
    if (wallpaper_state != WALLPAPER_IDLE || generated_update_pending || voice_input_is_recording()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = voice_input_start_with_callback(wallpaper_transcript_ready, NULL);
    if (result == ESP_OK) {
        generated_update_pending = false;
        wallpaper_state = WALLPAPER_RECORDING;
        set_wallpaper_status("RECORDING WALLPAPER IDEA - TAP STOP");
    }
    return result;
}

esp_err_t wallpaper_input_finish(void)
{
    if (wallpaper_state != WALLPAPER_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = voice_input_finish();
    if (result == ESP_OK) {
        set_wallpaper_status("RECOGNIZING WALLPAPER IDEA...");
    }
    return result;
}

bool wallpaper_input_take_reload_request(void)
{
    if (!reload_requested) {
        return false;
    }
    reload_requested = false;
    return true;
}

void wallpaper_input_request_reload(void) { reload_requested=true; }

bool wallpaper_input_has_pending_update(void)
{
    return generated_update_pending;
}

void wallpaper_input_mark_cached_available(void)
{
    if (wallpaper_state == WALLPAPER_IDLE && !generated_update_pending) {
        set_wallpaper_status("CURRENT WALLPAPER SAVED ON DEVICE");
    }
}

void wallpaper_input_mark_download_result(bool persisted)
{
    if (!generated_update_pending) {
        return;
    }
    if (persisted) {
        generated_update_pending = false;
        set_wallpaper_status("WALLPAPER SAVED ON DEVICE");
    } else {
        set_wallpaper_status("WALLPAPER UPDATE RETRYING");
    }
}

const char *wallpaper_input_status(void)
{
    return wallpaper_status;
}
