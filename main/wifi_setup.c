#include "wifi_setup.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#define PROFILE_COUNT 5
#define READY_BIT BIT0
#define CONNECTED_BIT BIT0
#define LOST_BIT BIT1
typedef struct { unsigned count; wifi_config_t items[PROFILE_COUNT]; } profiles_t;
typedef struct { int kind; wifi_config_t config; } command_t;
static profiles_t profiles;
static wifi_config_t working, candidate;
static EventGroupHandle_t app_events, events;
static QueueHandle_t commands;
static SemaphoreHandle_t guard;
static wifi_setup_state_t state;
static esp_netif_t *ap_netif;
static httpd_handle_t http;
static char session[33];
static int64_t expires, attempt_deadline, close_at, retry_at;
static bool testing, connecting;
static unsigned reconnect_index, failures;
extern const uint8_t setup_html_start[] asm("_binary_wifi_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_wifi_setup_html_end");

static int64_t seconds(void) { return esp_timer_get_time() / 1000000; }
static void status(const char *s)
{
    xSemaphoreTake(guard, portMAX_DELAY);
    strlcpy(state.status, s, sizeof(state.status));
    state.testing = testing;
    state.saved_count = profiles.count;
    xSemaphoreGive(guard);
}
void wifi_setup_state(wifi_setup_state_t *out)
{
    if (!guard) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(guard, portMAX_DELAY); *out = state; xSemaphoreGive(guard);
}
static void random_hex(char *out, size_t bytes)
{
    uint8_t raw[16]; esp_fill_random(raw, bytes);
    for (size_t i=0; i<bytes; ++i) snprintf(out+i*2, 3, "%02x", raw[i]);
}
static esp_err_t save_profile(const wifi_config_t *config)
{
    profiles_t next = profiles;
    unsigned index = next.count;
    for (unsigned i=0; i<next.count; ++i)
        if (!memcmp(next.items[i].sta.ssid, config->sta.ssid, 32)) { index=i; break; }
    if (index >= PROFILE_COUNT) index = PROFILE_COUNT-1;
    for (unsigned i=index; i>0; --i) next.items[i] = next.items[i-1];
    next.items[0] = *config;
    if (profiles.count < PROFILE_COUNT && index == profiles.count) ++next.count;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("wifi_profiles", NVS_READWRITE, &nvs);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(nvs, "profiles_v1", &next, sizeof(next));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    if (result == ESP_OK) {
        profiles = next;
        xSemaphoreTake(guard,portMAX_DELAY); state.saved_count=profiles.count; xSemaphoreGive(guard);
    }
    ESP_LOGI("wifi_setup","Profile persistence: %s (%u networks)",esp_err_to_name(result),profiles.count);
    return result;
}
static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(app_events, READY_BIT);
        xEventGroupSetBits(events, CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(app_events, READY_BIT);
        xEventGroupSetBits(events, LOST_BIT);
    }
}
static void connect_config(const wifi_config_t *config)
{
    connecting = true;
    esp_wifi_disconnect();
    xEventGroupClearBits(app_events, READY_BIT);
    xEventGroupClearBits(events, CONNECTED_BIT | LOST_BIT);
    /* All candidate writes stay in RAM. Only a successful connection is persisted. */
    wifi_config_t copy = *config;
    esp_wifi_set_config(WIFI_IF_STA, &copy);
    esp_wifi_connect();
    retry_at = seconds() + 10;
}
static bool local_request(httpd_req_t *req)
{
    struct sockaddr_in address; socklen_t size = sizeof(address);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&address, &size) != 0 ||
        address.sin_addr.s_addr != inet_addr("192.168.4.1")) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Setup is available only on the device hotspot");
        return false;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; form-action 'self'");
    return true;
}
static bool authenticated(httpd_req_t *req)
{
    if (!local_request(req)) return false;
    char token[40] = {0}, origin[80] = {0};
    httpd_req_get_hdr_value_str(req, "X-Setup-Token", token, sizeof(token));
    unsigned diff = strlen(token) ^ strlen(session);
    for (unsigned i=0; i<32; ++i) diff |= token[i] ^ session[i];
    size_t n = httpd_req_get_hdr_value_len(req, "Origin");
    if (n && (n >= sizeof(origin) || httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
              strcmp(origin, "http://192.168.4.1"))) diff |= 1;
    if (diff) { httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid setup session"); return false; }
    return true;
}
static esp_err_t json_response(httpd_req_t *req, cJSON *value)
{
    char *body = cJSON_PrintUnformatted(value); cJSON_Delete(value);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(req, body); free(body); return result;
}
static esp_err_t root_handler(httpd_req_t *req)
{
    if (!local_request(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)setup_html_start, setup_html_end-setup_html_start);
}
static esp_err_t session_handler(httpd_req_t *req)
{
    if (!local_request(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "token", session);
    return json_response(req, j);
}
static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    wifi_setup_state_t snapshot; wifi_setup_state(&snapshot);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "message", snapshot.status);
    cJSON_AddBoolToObject(j, "testing", snapshot.testing);
    return json_response(req, j);
}
static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    wifi_setup_state_t snapshot; wifi_setup_state(&snapshot);
    if (snapshot.testing) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Connection in progress");
    wifi_scan_config_t scan = {.scan_time.active = {.min=40, .max=80}};
    if (esp_wifi_scan_start(&scan, true) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi is busy; retry shortly");
    uint16_t count = 20;
    wifi_ap_record_t *aps = calloc(count, sizeof(*aps));
    if (!aps) { esp_wifi_clear_ap_list(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"); }
    esp_err_t result = esp_wifi_scan_get_ap_records(&count, aps);
    cJSON *j = cJSON_CreateArray();
    if (result == ESP_OK) for (unsigned i=0; i<count; ++i) {
        if (!aps[i].ssid[0]) continue;
        char ssid[33]; memcpy(ssid, aps[i].ssid, 32); ssid[32]=0;
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddBoolToObject(ap, "secured", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(j, ap);
    }
    free(aps); return json_response(req, j);
}
static esp_err_t connect_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    char type[40] = {0}; httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
    if (strncmp(type, "application/json", 16) || req->content_len <= 0 || req->content_len > 512)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    char body[513]; int used=0;
    while (used<req->content_len) {
        int n = httpd_req_recv(req, body+used, req->content_len-used);
        if (n <= 0) return ESP_FAIL;
        used += n;
    }
    body[used]=0;
    cJSON *j = cJSON_Parse(body), *ssid = cJSON_GetObjectItem(j,"ssid"), *password = cJSON_GetObjectItem(j,"password");
    esp_err_t result = cJSON_IsString(ssid) && cJSON_IsString(password) ?
        wifi_setup_connect(ssid->valuestring,password->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j); memset(body,0,sizeof(body));
    if (result != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Check network name/password, or wait for the current attempt");
    return httpd_resp_sendstr(req,"{}");
}
static void stop_setup(void)
{
    if (http) { httpd_stop(http); http=NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    memset(session,0,sizeof(session));
    xSemaphoreTake(guard,portMAX_DELAY); state.active=false; memset(state.password,0,sizeof(state.password)); xSemaphoreGive(guard);
    close_at=0;
}
static esp_err_t start_setup(void)
{
    wifi_config_t config = {0};
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_AP,mac);
    snprintf((char *)config.ap.ssid,sizeof(config.ap.ssid),"RoundClock-%02X%02X",mac[4],mac[5]);
    random_hex((char *)config.ap.password,8); random_hex(session,16);
    config.ap.ssid_len=strlen((char *)config.ap.ssid); config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection=1; config.ap.channel=1; config.ap.pmf_cfg.capable=true;
    if (!ap_netif) ap_netif=esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_ERR_NO_MEM;
    esp_err_t result=esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result==ESP_OK) result=esp_wifi_set_config(WIFI_IF_AP,&config);
    httpd_config_t h=HTTPD_DEFAULT_CONFIG(); h.stack_size=6144;
    h.task_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    h.max_open_sockets=3; h.lru_purge_enable=true; h.recv_wait_timeout=4; h.send_wait_timeout=4;
    if (result==ESP_OK) result=httpd_start(&http,&h);
    if (result==ESP_OK) {
        const httpd_uri_t routes[]={
            {.uri="/",.method=HTTP_GET,.handler=root_handler},
            {.uri="/session",.method=HTTP_GET,.handler=session_handler},
            {.uri="/status",.method=HTTP_GET,.handler=status_handler},
            {.uri="/networks",.method=HTTP_GET,.handler=scan_handler},
            {.uri="/connect",.method=HTTP_POST,.handler=connect_handler},
        };
        for(unsigned i=0;i<sizeof(routes)/sizeof(routes[0]) && result==ESP_OK;++i)
            result=httpd_register_uri_handler(http,&routes[i]);
    }
    if (result!=ESP_OK) { stop_setup(); return result; }
    xSemaphoreTake(guard,portMAX_DELAY);
    state.active=true;
    strlcpy(state.ssid,(char *)config.ap.ssid,sizeof(state.ssid));
    strlcpy(state.password,(char *)config.ap.password,sizeof(state.password));
    xSemaphoreGive(guard);
    expires=seconds()+300; close_at=0;
    status("扫码连接热点，再打开下方网址");
    return ESP_OK;
}
static void rollback(void)
{
    testing=false; attempt_deadline=0; connecting=false;
    if (working.sta.ssid[0]) connect_config(&working);
    else esp_wifi_disconnect();
    status("连接失败，已保留原网络，请重试");
}
static void setup_task(void *ctx)
{
    (void)ctx;
    for (;;) {
        command_t cmd;
        if (xQueueReceive(commands,&cmd,pdMS_TO_TICKS(300))==pdTRUE) {
            if (cmd.kind==1 && !state.active) {
                if (start_setup()!=ESP_OK) status("启动失败，请稍后重试");
            } else if (cmd.kind==2) {
                if(testing) rollback();
                stop_setup();
            } else if(cmd.kind==3 && !testing) {
                candidate=cmd.config; testing=true; attempt_deadline=seconds()+25;
                connect_config(&candidate); status("正在连接，请稍候...");
            }
            memset(&cmd,0,sizeof(cmd));
        }
        EventBits_t bits=xEventGroupClearBits(events,CONNECTED_BIT|LOST_BIT);
        wifi_ap_record_t ap;
        if ((bits&CONNECTED_BIT) && esp_wifi_sta_get_ap_info(&ap)==ESP_OK) {
            connecting=false; failures=0; reconnect_index=0;
            wifi_config_t actual; esp_wifi_get_config(WIFI_IF_STA,&actual);
            if (testing && !memcmp(ap.ssid,candidate.sta.ssid,32)) {
                testing=false; working=actual;
                if(save_profile(&working)==ESP_OK) {
                    status("连接成功，已保存；热点即将关闭");
                    if(state.active) close_at=seconds()+8;
                } else status("已连接，但保存失败，请重试");
            } else if(!testing) {
                working=actual;
                if(!profiles.count) save_profile(&working);
            }
        }
        if(testing && seconds()>=attempt_deadline) rollback();
        if((bits&LOST_BIT) && !connecting) { connecting=true; retry_at=seconds()+2; }
        if(connecting && seconds()>=retry_at) {
            if(testing) esp_wifi_connect();
            else if(!state.active && profiles.count && ++failures>=2) {
                reconnect_index=(reconnect_index+1)%profiles.count;
                connect_config(&profiles.items[reconnect_index]); failures=0;
            } else if(working.sta.ssid[0]) esp_wifi_connect();
            retry_at=seconds()+10;
        }
        if(state.active && (seconds()>=expires || (close_at && seconds()>=close_at))) {
            if(testing) rollback();
            stop_setup();
        }
    }
}
esp_err_t wifi_setup_init(EventGroupHandle_t connected_events,const wifi_config_t *initial)
{
    app_events=connected_events; events=xEventGroupCreate(); guard=xSemaphoreCreateMutex();
    commands=xQueueCreate(3,sizeof(command_t));
    if(!events||!guard||!commands) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    if(nvs_open("wifi_profiles",NVS_READONLY,&nvs)==ESP_OK) {
        size_t size=sizeof(profiles);
        if(nvs_get_blob(nvs,"profiles_v1",&profiles,&size)!=ESP_OK || size!=sizeof(profiles) || profiles.count>PROFILE_COUNT)
            memset(&profiles,0,sizeof(profiles));
        nvs_close(nvs);
    }
    working=profiles.count?profiles.items[0]:*initial;
    status("点击手机配网");
    /* The old flash configuration remains untouched as a migration fallback. */
    esp_err_t r=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(r==ESP_OK) r=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,wifi_event,NULL);
    if(r==ESP_OK) r=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL);
    if(r==ESP_OK) r=esp_wifi_set_mode(WIFI_MODE_STA);
    if(r==ESP_OK && working.sta.ssid[0]) r=esp_wifi_set_config(WIFI_IF_STA,&working);
    if(r==ESP_OK) r=esp_wifi_start();
    if(r!=ESP_OK) return r;
    if(working.sta.ssid[0]) { connecting=true; retry_at=seconds()+10; esp_wifi_connect(); }
    return xTaskCreateWithCaps(setup_task,"wifi_setup",4096,NULL,3,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
esp_err_t wifi_setup_open(void)
{
    if(!commands) return ESP_ERR_INVALID_STATE;
    command_t cmd={.kind=1};
    return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}
void wifi_setup_close(void)
{
    if(commands) { command_t cmd={.kind=2}; xQueueSend(commands,&cmd,0); }
}
esp_err_t wifi_setup_connect(const char *ssid,const char *password)
{
    size_t s=strlen(ssid), p=strlen(password);
    if(!commands || !s || s>32 || (p && p<8) || p>63) return ESP_ERR_INVALID_ARG;
    wifi_setup_state_t snapshot; wifi_setup_state(&snapshot);
    if(snapshot.testing || uxQueueMessagesWaiting(commands)) return ESP_ERR_INVALID_STATE;
    command_t cmd={.kind=3};
    memcpy(cmd.config.sta.ssid,ssid,s); memcpy(cmd.config.sta.password,password,p);
    cmd.config.sta.threshold.authmode=p?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
    cmd.config.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
    return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_setup_test_connection(bool invalid)
{
    if (invalid) return wifi_setup_connect("RoundClock-Acceptance-Missing", "not-a-real-password");
    wifi_config_t current;
    if (esp_wifi_get_config(WIFI_IF_STA,&current)!=ESP_OK) return ESP_FAIL;
    char ssid[33], password[65];
    memcpy(ssid,current.sta.ssid,32); ssid[32]=0;
    memcpy(password,current.sta.password,64); password[64]=0;
    esp_err_t result=wifi_setup_connect(ssid,password);
    memset(password,0,sizeof(password)); return result;
}
