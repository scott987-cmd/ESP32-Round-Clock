#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music_input.h"
#include "notification_center.h"
#include "voice_input.h"
#include "audio_bus.h"
#include "device_api.h"
#include "service_config.h"

#define MUSIC_MAX_PCM_BYTES (12 * 1024 * 1024)
extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static music_input_state_t state;
static bool busy;
static audio_lease_t playback_lease;
static unsigned playback_workers;
typedef struct {audio_lease_t lease;uint32_t sequence;char url[256];} playback_job_t;
static int64_t next_sync;
static char music_status[80] = "READY FOR MUSIC IDEA";
static char music_prompt[1024], music_prefix[512];
static unsigned recording_station;
static const char *station_ids[] = {"", "sky", "aurora", "energy"};
static const char *station_prompts[] = {
    "", "宁静星空下的专注纯音乐，轻柔钢琴、温暖合成器，没有人声。",
    "极光下的舒缓助眠纯音乐，缓慢空灵、温暖柔和，没有人声。",
    "明亮有活力的电子纯音乐，乐观、清晰节奏，没有人声。"
};
enum { TASK_SYNC, TASK_CREATE };

static const char *str(cJSON *j, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
static bool valid_id(const char *id)
{
    return id && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32;
}
music_input_state_t music_input_state(void)
{
    taskENTER_CRITICAL(&mux); music_input_state_t copy = state; copy.busy = busy;
    audio_lease_t lease=playback_lease;taskEXIT_CRITICAL(&mux);
    copy.playing=copy.playing&&audio_bus_is_current(lease);
    return copy;
}
static void set_status(const char *text)
{
    taskENTER_CRITICAL(&mux); strlcpy(music_status, text, sizeof(music_status)); taskEXIT_CRITICAL(&mux);
}
static void load_catalogue(cJSON *root)
{
    cJSON *stations = cJSON_GetObjectItem(root, "stations");
    taskENTER_CRITICAL(&mux);
    for (unsigned i = 0; i < 4; i++) {
        cJSON *item = cJSON_GetObjectItem(i ? stations : root, i ? station_ids[i] : "latest");
        const char *id = str(item, "artworkId");
        strlcpy(state.saved[i], valid_id(id) ? id : "", sizeof(state.saved[i]));
    }
    taskEXIT_CRITICAL(&mux);
}
static esp_err_t get_json(const char *path, const char *body, cJSON **root)
{
    char *response = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) return ESP_ERR_NO_MEM;
    size_t length;
    esp_err_t result = device_api(path, body, response, 4096, &length);
    *root = result == ESP_OK ? cJSON_Parse(response) : NULL;
    heap_caps_free(response);
    if (result == ESP_OK && !cJSON_IsObject(*root)) result = ESP_ERR_INVALID_RESPONSE;
    return result;
}
static esp_err_t synchronize(void)
{
    cJSON *root = NULL;
    esp_err_t result = get_json("/v1/music/status", NULL, &root);
    if (result != ESP_OK) { cJSON_Delete(root); return result; }
    load_catalogue(root);
    cJSON *job = cJSON_GetObjectItem(root, "job");
    char id[33]; strlcpy(id, str(job, "requestId"), sizeof(id));
    bool working = !strcmp(str(job, "status"), "working");
    bool failed = !strcmp(str(job, "status"), "failed");
    cJSON_Delete(root);
    if (working && valid_id(id)) {
        char path[96]; snprintf(path, sizeof(path), "/v1/music/status?job=%s", id);
        set_status("CREATING MUSIC...");
        for (unsigned n = 0; n < 150; n++) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            root = NULL; result = get_json(path, NULL, &root);
            if (result != ESP_OK) { cJSON_Delete(root); return result; }
            working = !strcmp(str(root, "status"), "working");
            failed = strcmp(str(root, "status"), "done") != 0;
            cJSON_Delete(root);
            if (!working) break;
        }
        if (working) return ESP_ERR_TIMEOUT;
        root = NULL; result = get_json("/v1/music/status", NULL, &root);
        if (result == ESP_OK) load_catalogue(root);
        cJSON_Delete(root);
        if (result != ESP_OK) return result;
        notification_post("music", failed ? "音乐生成失败，已保存作品仍可播放" : "音乐已保存，点此播放", 10, failed ? NOTICE_FAILED : NOTICE_DONE);
    }
    set_status(failed ? "MUSIC CREATION FAILED" : music_input_state().saved[0][0] ? "MUSIC READY - TAP PLAY" : "READY FOR MUSIC IDEA");
    return ESP_OK;
}
static esp_err_t create_music(void)
{
    char id[33], path[96];
    snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "requestId", id);
    cJSON_AddStringToObject(body, "prompt", music_prompt);
    cJSON_AddStringToObject(body, "station", station_ids[recording_station]);
    char *request = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    if (!request) return ESP_ERR_NO_MEM;
    cJSON *response = NULL;
    esp_err_t result = get_json("/v1/music", request, &response);
    cJSON_free(request); cJSON_Delete(response);
    memset(music_prompt, 0, sizeof(music_prompt));
    if (result != ESP_OK) {
        /* A lost acknowledgement is not permission to charge for a second song. */
        snprintf(path, sizeof(path), "/v1/music/status?job=%s", id);
        response = NULL; result = get_json(path, NULL, &response); cJSON_Delete(response);
    }
    return result == ESP_OK ? synchronize() : result;
}
static esp_err_t play_music(playback_job_t *job)
{
    if(!audio_bus_is_current(job->lease))return ESP_ERR_INVALID_STATE;
    esp_err_t result;
    esp_http_client_config_t config = {
        .url = job->url, .timeout_ms = 15000, .buffer_size = 2048, .disable_auto_redirect = true,
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    char auth[192]; snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth);
    result = esp_http_client_open(client, 0);
    if (result == ESP_OK) {
        int64_t length = esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200 || length <= 0 || length > MUSIC_MAX_PCM_BYTES || length % 2) result = ESP_FAIL;
        else {
            uint8_t buffer[2048]; int received = 0, carry = 0;
            while (audio_bus_is_current(job->lease) && (received = esp_http_client_read(client, (char *)buffer + carry, sizeof(buffer) - carry)) > 0) {
                int total = received + carry, aligned = total & ~1;
                if (aligned && audio_bus_write(job->lease,buffer,aligned) != ESP_OK) { result = ESP_FAIL; break; }
                taskENTER_CRITICAL(&mux);if(state.sequence==job->sequence)state.written += aligned;taskEXIT_CRITICAL(&mux);
                carry = total & 1; if (carry) buffer[0] = buffer[aligned];
            }
            if (audio_bus_is_current(job->lease) && (received < 0 || carry || !esp_http_client_is_complete_data_received(client))) result = ESP_FAIL;
        }
    }
    esp_http_client_close(client); esp_http_client_cleanup(client);
    return result;
}
static void playback_worker(void *arg)
{
    playback_job_t *job=arg;
    esp_err_t result=play_music(job);
    bool interrupted=!audio_bus_is_current(job->lease);
    audio_bus_release(job->lease);
    taskENTER_CRITICAL(&mux);
    if(state.sequence==job->sequence){
        state.playing=false;
        if(!busy)strlcpy(music_status,interrupted||result==ESP_OK?"MUSIC READY - TAP PLAY":"MUSIC PLAYBACK FAILED",sizeof(music_status));
    }
    playback_workers--;
    taskEXIT_CRITICAL(&mux);
    free(job);vTaskDeleteWithCaps(NULL);
}
static void worker(void *arg)
{
    unsigned action = (unsigned)(uintptr_t)arg;
    esp_err_t result = action == TASK_CREATE ? create_music() : synchronize();
    if (result != ESP_OK) {
        set_status(action == TASK_CREATE ? "MUSIC CREATION FAILED" : "MUSIC SYNC FAILED");
        if (action == TASK_CREATE) notification_post("music", "音乐生成未完成，请查看网络及作品库", recording_station ? 11 : 10, NOTICE_FAILED);
    } else if (action == TASK_CREATE) {
        taskENTER_CRITICAL(&mux); bool ready = !strcmp(music_status, "MUSIC READY - TAP PLAY"); taskEXIT_CRITICAL(&mux);
        if (ready) notification_post("music", "音乐已保存，点此播放", recording_station ? 11 : 10, NOTICE_DONE);
    }
    taskENTER_CRITICAL(&mux);
    busy = false;
    next_sync = esp_timer_get_time() + (result == ESP_OK ? 30000000 : 10000000);
    taskEXIT_CRITICAL(&mux);
    vTaskDeleteWithCaps(NULL);
}
static esp_err_t launch(unsigned action)
{
    if (xTaskCreateWithCaps(worker, "music", 8192, (void *)(uintptr_t)action, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return ESP_OK;
    taskENTER_CRITICAL(&mux); busy = false; taskEXIT_CRITICAL(&mux);
    set_status("MUSIC SYNC FAILED"); return ESP_ERR_NO_MEM;
}
void music_input_refresh(void)
{
    if (voice_input_is_recording() || voice_input_is_processing()) return;
    taskENTER_CRITICAL(&mux);
    bool run = !busy && !state.recording && !state.playing && esp_timer_get_time() >= next_sync;
    if (run) busy = true;
    taskEXIT_CRITICAL(&mux);
    if (run) launch(TASK_SYNC);
}
static void transcript_ready(const char *text, void *context)
{
    (void)context;
    taskENTER_CRITICAL(&mux); state.recording = false; taskEXIT_CRITICAL(&mux);
    if (!text || !*text || strlen(text) + strlen(music_prefix) + 2 > sizeof(music_prompt)) {
        set_status("MUSIC IDEA FAILED");
        taskENTER_CRITICAL(&mux); busy = false; next_sync = esp_timer_get_time() + 30000000; taskEXIT_CRITICAL(&mux);
        return;
    }
    strlcpy(music_prompt, music_prefix, sizeof(music_prompt));
    if (music_prefix[0]) strlcat(music_prompt, " ", sizeof(music_prompt));
    strlcat(music_prompt, text, sizeof(music_prompt));
    set_status("CREATING MUSIC...");
    notification_post("music", "音乐正在生成，可继续使用其他应用", recording_station ? 11 : 10, NOTICE_WORKING);
    launch(TASK_CREATE);
}
static bool reserve(void)
{
    taskENTER_CRITICAL(&mux); bool ok = !busy && !state.recording;
    if (ok) busy = true;
    taskEXIT_CRITICAL(&mux); return ok;
}
esp_err_t music_input_start(void) { return music_input_start_radio(0); }
esp_err_t music_input_start_with_prefix(const char *prefix)
{
    if (!reserve()) return ESP_ERR_INVALID_STATE;
    strlcpy(music_prefix, prefix ? prefix : "", sizeof(music_prefix));
    taskENTER_CRITICAL(&mux); state.recording = true; taskEXIT_CRITICAL(&mux);
    esp_err_t result = voice_input_start_with_callback(transcript_ready, NULL);
    if (result == ESP_OK) set_status("RECORDING MUSIC IDEA - TAP STOP");
    else { taskENTER_CRITICAL(&mux); busy = false; state.recording = false; taskEXIT_CRITICAL(&mux); }
    return result;
}
esp_err_t music_input_start_radio(unsigned station)
{
    if (station > 3 || music_input_state().busy) return ESP_ERR_INVALID_STATE;
    recording_station = station;
    return music_input_start_with_prefix(station_prompts[station]);
}
esp_err_t music_input_generate_radio(unsigned station)
{
    if (!station || station > 3 || !reserve()) return ESP_ERR_INVALID_STATE;
    recording_station = station;
    strlcpy(music_prompt, station_prompts[station], sizeof(music_prompt));
    set_status("CREATING MUSIC...");
    notification_post("music", "频道音乐正在生成，可切换其他应用", 11, NOTICE_WORKING);
    return launch(TASK_CREATE);
}
esp_err_t music_input_finish(void)
{
    if (!music_input_state().recording) return ESP_ERR_INVALID_STATE;
    set_status("RECOGNIZING MUSIC IDEA...");
    return voice_input_finish();
}
esp_err_t music_input_play_artwork(const char *id)
{
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;
    playback_job_t *job=calloc(1,sizeof(*job));if(!job)return ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&mux);
    /* Displaced HTTP workers close themselves; cap sockets during rapid taps. */
    if(busy||state.recording||playback_workers>=3){taskEXIT_CRITICAL(&mux);free(job);return ESP_ERR_INVALID_STATE;}
    playback_workers++;
    job->lease=playback_lease=audio_bus_request();
    state.playing=true;state.written=0;job->sequence=++state.sequence;
    taskEXIT_CRITICAL(&mux);
    snprintf(job->url,sizeof(job->url),CLOCK_API_BASE "/v1/library?id=%s&part=data",id);
    set_status("PLAYING MUSIC...");
    if(xTaskCreateWithCaps(playback_worker,"music_play",8192,job,4,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS)return ESP_OK;
    audio_bus_release(job->lease);
    taskENTER_CRITICAL(&mux);playback_workers--;
    if(state.sequence==job->sequence){state.playing=false;if(!busy)strlcpy(music_status,"MUSIC PLAYBACK FAILED",sizeof(music_status));}
    taskEXIT_CRITICAL(&mux);
    free(job);return ESP_ERR_NO_MEM;
}
esp_err_t music_input_play_station(unsigned station)
{
    if (station > 3) return ESP_ERR_INVALID_ARG;
    music_input_state_t copy = music_input_state();
    return copy.saved[station][0] ? music_input_play_artwork(copy.saved[station]) : ESP_ERR_NOT_FOUND;
}
esp_err_t music_input_play(void) { return music_input_play_station(0); }
void music_input_stop(void)
{
    taskENTER_CRITICAL(&mux);audio_lease_t lease=playback_lease;state.playing=false;
    if(!busy)strlcpy(music_status,"MUSIC READY - TAP PLAY",sizeof(music_status));
    taskEXIT_CRITICAL(&mux);audio_bus_release(lease);
}
void music_input_set_volume(uint8_t volume) { audio_bus_set_volume(volume); }
bool music_input_is_recording(void) { return music_input_state().recording; }
const char *music_input_status(void)
{
    /* UI consumes on the LVGL task; return a stable copy of worker-owned status. */
    static char copy[80];
    taskENTER_CRITICAL(&mux); strlcpy(copy, music_status, sizeof(copy)); taskEXIT_CRITICAL(&mux);
    return copy;
}
