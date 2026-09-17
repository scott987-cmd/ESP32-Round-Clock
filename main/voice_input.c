#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "audio_bus.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "voice_input.h"
#include "service_config.h"

#define VOICE_SAMPLE_RATE 16000
#define VOICE_MAX_SECONDS 20
#define VOICE_MAX_BYTES (VOICE_SAMPLE_RATE * 2 * VOICE_MAX_SECONDS)
#define VOICE_READ_BYTES 1024
#define VOICE_RESPONSE_BYTES 1536

extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");

typedef enum {
    VOICE_IDLE,
    VOICE_RECORDING,
    VOICE_UPLOADING,
} voice_state_t;

static const char *TAG = "voice_input";
static esp_codec_dev_handle_t microphone_codec;
static TaskHandle_t voice_task_handle;
static uint8_t *recording_buffer;
static volatile voice_state_t voice_state = VOICE_IDLE;
static volatile bool finish_requested;
static volatile uint8_t current_voice_level;
static char voice_status[VOICE_RESPONSE_BYTES] = "READY FOR ONLINE VOICE";
static voice_input_transcript_cb_t transcript_callback;
static void *transcript_context;

static void set_voice_status(const char *status)
{
    if (status != NULL) {
        strlcpy(voice_status, status, sizeof(voice_status));
        ESP_LOGI(TAG, "%s", voice_status);
    }
}

static uint8_t audio_level(const uint8_t *samples, size_t length)
{
    const int16_t *pcm = (const int16_t *)samples;
    size_t count = length / sizeof(*pcm);
    if (pcm == NULL || count == 0) {
        return 0;
    }
    uint32_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = pcm[i];
        total += (uint32_t)(sample < 0 ? -sample : sample);
    }
    uint32_t average = total / count;
    return (uint8_t)(average >= 9000 ? 100 : (average * 100) / 9000);
}

static esp_err_t initialize_microphone(void)
{
    if (microphone_codec != NULL) {
        return ESP_OK;
    }
    microphone_codec = bsp_audio_codec_microphone_init();
    if (microphone_codec == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = VOICE_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(microphone_codec, &sample_info) != ESP_CODEC_DEV_OK) {
        microphone_codec = NULL;
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_in_gain(microphone_codec, 30.0f) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Unable to set microphone gain");
    }
    return ESP_OK;
}

static esp_err_t transcribe_online(const uint8_t *audio, size_t length,
                                   char *transcript, size_t transcript_size)
{
    if (audio == NULL || length == 0 || length > VOICE_MAX_BYTES || (length & 1U)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = CLOCK_TRANSCRIBE_URL,
        .timeout_ms = 90000,
        .buffer_size = 2048,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/l16;rate=16000;channels=1");
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t result = esp_http_client_open(client, (int)length);
    size_t offset = 0;
    while (result == ESP_OK && offset < length) {
        int written = esp_http_client_write(client, (const char *)audio + offset,
                                            (int)(length - offset));
        if (written <= 0) {
            result = ESP_FAIL;
            break;
        }
        offset += (size_t)written;
    }

    char response[VOICE_RESPONSE_BYTES] = {0};
    if (result == ESP_OK) {
        int64_t response_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200 || response_length >= (int64_t)sizeof(response)) {
            ESP_LOGE(TAG, "Transcribe HTTP status=%d length=%lld", status, response_length);
            result = ESP_FAIL;
        } else {
            int total = 0;
            while (total < (int)sizeof(response) - 1) {
                int received = esp_http_client_read(client, response + total,
                                                    sizeof(response) - 1 - total);
                if (received < 0) {
                    result = ESP_FAIL;
                    break;
                }
                if (received == 0) {
                    break;
                }
                total += received;
            }
            response[total] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        return result;
    }

    cJSON *root = cJSON_Parse(response);
    cJSON *text_item = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text_item) || text_item->valuestring == NULL ||
        text_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Online recognition completed (%u UTF-8 bytes)",
             (unsigned)strlen(text_item->valuestring));
    if (transcript != NULL && transcript_size > 0) {
        strlcpy(transcript, text_item->valuestring, transcript_size);
    }
    set_voice_status("ONLINE VOICE RECOGNIZED");
    cJSON_Delete(root);
    return ESP_OK;
}

static void deliver_transcript(const char *text)
{
    voice_input_transcript_cb_t callback = transcript_callback;
    void *context = transcript_context;
    transcript_callback = NULL;
    transcript_context = NULL;
    voice_state = VOICE_IDLE;
    if (callback != NULL) callback(text, context);
}

static void voice_task(void *context)
{
    (void)context;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (voice_state != VOICE_RECORDING) {
            continue;
        }
        if (recording_buffer == NULL) {
            recording_buffer = heap_caps_malloc(VOICE_MAX_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (recording_buffer == NULL || initialize_microphone() != ESP_OK) {
            heap_caps_free(recording_buffer);
            recording_buffer = NULL;
            audio_bus_release();
            voice_state = VOICE_IDLE;
            set_voice_status("MICROPHONE FAILED");
            deliver_transcript(NULL);
            continue;
        }

        size_t recorded = 0;
        int64_t capture_started_us = esp_timer_get_time();
        while (!finish_requested && recorded + VOICE_READ_BYTES <= VOICE_MAX_BYTES) {
            int read_result = esp_codec_dev_read(microphone_codec,
                                                 recording_buffer + recorded,
                                                 VOICE_READ_BYTES);
            if (read_result != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "Microphone read failed: %d", read_result);
                break;
            }
            current_voice_level = audio_level(recording_buffer + recorded, VOICE_READ_BYTES);
            recorded += VOICE_READ_BYTES;
        }
        current_voice_level = 0;
        audio_bus_release();
        /* PCM length is bytes, not samples. Warm DMA can also deliver queued
         * frames immediately, so require half a second of real capture time. */
        if (recorded < VOICE_SAMPLE_RATE * sizeof(int16_t) / 2 ||
            esp_timer_get_time() - capture_started_us < 500000) {
            heap_caps_free(recording_buffer);
            recording_buffer = NULL;
            set_voice_status("RECORDING TOO SHORT");
            deliver_transcript(NULL);
            continue;
        }

        voice_state = VOICE_UPLOADING;
        set_voice_status("RECOGNIZING ONLINE...");
        char transcript[VOICE_RESPONSE_BYTES] = {0};
        esp_err_t result = transcribe_online(recording_buffer, recorded,
                                             transcript, sizeof(transcript));
        /* The PCM is no longer used once the synchronous upload returns.
         * Do not retain a 640 KB recording buffer between applications. */
        heap_caps_free(recording_buffer);
        recording_buffer = NULL;
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Online voice input failed: %s", esp_err_to_name(result));
            set_voice_status("ONLINE VOICE FAILED");
        }
        deliver_transcript(result == ESP_OK ? transcript : NULL);
    }
}

esp_err_t voice_input_start(void)
{
    return voice_input_start_with_callback(NULL, NULL);
}

esp_err_t voice_input_start_with_callback(voice_input_transcript_cb_t callback,
                                          void *context)
{
    if (voice_state != VOICE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_bus_try_acquire()) return ESP_ERR_INVALID_STATE;
    if (voice_task_handle == NULL) {
        BaseType_t created = xTaskCreatePinnedToCore(voice_task, "voice_input", 8192, NULL, 4,
                                                     &voice_task_handle, 1);
        if (created != pdPASS) {
            audio_bus_release();
            voice_task_handle = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    finish_requested = false;
    current_voice_level = 0;
    transcript_callback = callback;
    transcript_context = context;
    voice_state = VOICE_RECORDING;
    set_voice_status("RECORDING - TAP STOP");
    xTaskNotifyGive(voice_task_handle);
    return ESP_OK;
}

esp_err_t voice_input_finish(void)
{
    if (voice_state != VOICE_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    finish_requested = true;
    set_voice_status("FINISHING RECORDING...");
    return ESP_OK;
}

bool voice_input_is_recording(void)
{
    return voice_state == VOICE_RECORDING;
}

bool voice_input_is_processing(void)
{
    return voice_state == VOICE_UPLOADING;
}

uint8_t voice_input_level(void)
{
    return current_voice_level;
}

const char *voice_input_status(void)
{
    return voice_status;
}
