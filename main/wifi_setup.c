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
#define SCAN_BIT BIT2
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
static wifi_ap_record_t scan_rows[WIFI_SETUP_SCAN_LIMIT];
static uint16_t scan_count;
static uint32_t scan_revision;
static bool scanning;
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
static int64_t test_offline_until;
static volatile bool test_service_blocked;
#endif
extern const uint8_t setup_html_start[] asm("_binary_wifi_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_wifi_setup_html_end");

static int64_t seconds(void) { return esp_timer_get_time() / 1000000; }
bool wifi_setup_has_ip(void) {return app_events&&(xEventGroupGetBits(app_events)&READY_BIT);}
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
static esp_err_t setup_credentials(wifi_config_t *config,const uint8_t mac[6])
{
    uint8_t secret[10]={0};size_t size=sizeof(secret);nvs_handle_t nvs;
    esp_err_t result=nvs_open("wifi_setup",NVS_READWRITE,&nvs);
    if(result!=ESP_OK)return result;
    result=nvs_get_blob(nvs,"ap_secret",secret,&size);
    if(result!=ESP_OK||size!=sizeof(secret)){
        esp_fill_random(secret,sizeof(secret));
        result=nvs_set_blob(nvs,"ap_secret",secret,sizeof(secret));
        if(result==ESP_OK)result=nvs_commit(nvs);
    }
    nvs_close(nvs);
    if(result==ESP_OK){
        snprintf((char *)config->ap.ssid,sizeof(config->ap.ssid),"RoundClock-%02X%02X-%02X%02X",
                 mac[4],mac[5],secret[8],secret[9]);
        for(size_t i=0;i<8;i++)snprintf((char *)config->ap.password+i*2,3,"%02x",secret[i]);
    }
    memset(secret,0,sizeof(secret));return result;
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
    } else if(base==WIFI_EVENT&&id==WIFI_EVENT_SCAN_DONE){
        xEventGroupSetBits(events,SCAN_BIT);
    } else if(base==WIFI_EVENT&&(id==WIFI_EVENT_AP_STACONNECTED||id==WIFI_EVENT_AP_STADISCONNECTED)){
        wifi_sta_list_t stations={0};esp_wifi_ap_get_sta_list(&stations);
        xSemaphoreTake(guard,portMAX_DELAY);state.clients=stations.num;bool active=state.active;xSemaphoreGive(guard);
        if(id==WIFI_EVENT_AP_STACONNECTED)status("手机已连接热点，请打开下方网址");
        else if(active)status("手机已断开热点，请重新扫码");
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
static bool hotspot_peer(int fd)
{
    if(!ap_netif)return false;
    struct sockaddr_storage peer={0};socklen_t size=sizeof(peer);
    esp_netif_ip_info_t local;
    if(getpeername(fd,(struct sockaddr *)&peer,&size)!=0||
       esp_netif_get_ip_info(ap_netif,&local)!=ESP_OK)return false;
    uint32_t address=0;
    if(peer.ss_family==AF_INET)address=((struct sockaddr_in *)&peer)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    else if(peer.ss_family==AF_INET6&&IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&peer)->sin6_addr))
        memcpy(&address,&((struct sockaddr_in6 *)&peer)->sin6_addr.s6_addr[12],sizeof(address));
#endif
    else return false;
    uint32_t network=local.ip.addr&local.netmask.addr;
    uint32_t broadcast=network|~local.netmask.addr;
    return (address&local.netmask.addr)==network&&address!=network&&
        address!=broadcast&&address!=local.ip.addr;
}
static bool local_request(httpd_req_t *req)
{
    if (!hotspot_peer(httpd_req_to_sockfd(req))) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Setup is available only on the device hotspot");
        return false;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; form-action 'self'");
    return true;
}
static bool canonical_host(httpd_req_t *req)
{
    char host[32]={0};size_t length=httpd_req_get_hdr_value_len(req,"Host");
    return length>0&&length<sizeof(host)&&httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))==ESP_OK&&
        (!strcmp(host,"192.168.4.1")||!strcmp(host,"192.168.4.1:80"));
}
static esp_err_t redirect_to_setup(httpd_req_t *req)
{
    if(!local_request(req))return ESP_OK;
    httpd_resp_set_status(req,"302 Found");
    httpd_resp_set_hdr(req,"Location","http://192.168.4.1/");
    return httpd_resp_send(req,NULL,0);
}
static esp_err_t captive_not_found(httpd_req_t *req,httpd_err_code_t error)
{
    (void)error;return redirect_to_setup(req);
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
    if(!canonical_host(req))return redirect_to_setup(req);
    status("配置页面已打开，请选择无线网络");
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
    cJSON_AddBoolToObject(j, "connected", wifi_setup_has_ip());
    cJSON_AddBoolToObject(j, "relay", snapshot.relay);
    return json_response(req, j);
}
static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_OK;
    wifi_setup_state_t snapshot; wifi_setup_state(&snapshot);
    if (snapshot.testing) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Connection in progress");
    uint32_t revision=0;uint16_t count=0;
    wifi_setup_scan_read(NULL,&count,&revision);
    if (wifi_setup_scan() != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi is busy; retry shortly");
    wifi_ap_record_t *aps = calloc(WIFI_SETUP_SCAN_LIMIT,sizeof(*aps));
    if (!aps)return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    uint32_t current=revision;
    for(unsigned i=0;i<60&&current==revision;i++){vTaskDelay(pdMS_TO_TICKS(100));count=WIFI_SETUP_SCAN_LIMIT;wifi_setup_scan_read(aps,&count,&current);}
    if(current==revision){free(aps);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Scan timed out; retry shortly");}
    cJSON *j = cJSON_CreateArray();
    for (unsigned i=0; i<count; ++i) {
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
    if(ap_netif)esp_netif_napt_disable(ap_netif);
    if (http) { httpd_stop(http); http=NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    memset(session,0,sizeof(session));
    xSemaphoreTake(guard,portMAX_DELAY); state.active=false;state.relay=false;state.clients=0;memset(state.password,0,sizeof(state.password));xSemaphoreGive(guard);
    close_at=0;
}
static esp_err_t portal_handler(httpd_req_t *req)
{
    if(!authenticated(req))return ESP_OK;
    if(req->content_len>32||wifi_setup_portal()!=ESP_OK)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Connect the public Wi-Fi first");
    return httpd_resp_sendstr(req,"{}");
}
static esp_err_t start_setup(void)
{
    wifi_config_t config = {0};
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_AP,mac);
    esp_err_t result=setup_credentials(&config,mac);
    if(result!=ESP_OK)return result;
    random_hex(session,16);
    config.ap.ssid_len=strlen((char *)config.ap.ssid); config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection=1; config.ap.channel=1; config.ap.pmf_cfg.capable=true;
    if (!ap_netif) ap_netif=esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_ERR_NO_MEM;
    result=esp_wifi_set_mode(WIFI_MODE_APSTA);
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
            {.uri="/portal",.method=HTTP_POST,.handler=portal_handler},
        };
        for(unsigned i=0;i<sizeof(routes)/sizeof(routes[0]) && result==ESP_OK;++i)
            result=httpd_register_uri_handler(http,&routes[i]);
        if(result==ESP_OK)result=httpd_register_err_handler(http,HTTPD_404_NOT_FOUND,captive_not_found);
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
static esp_err_t start_relay(void)
{
    /* Follow ESP-IDF's softap_sta example. The phone uses its own browser and
     * authenticates through this station; never collect/proxy login credentials. */
    esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!sta||!ap_netif||!wifi_setup_has_ip())return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t upstream,local;esp_netif_dns_info_t dns;
    if(esp_netif_get_ip_info(sta,&upstream)!=ESP_OK||esp_netif_get_ip_info(ap_netif,&local)!=ESP_OK||
       (upstream.ip.addr&upstream.netmask.addr)==(local.ip.addr&upstream.netmask.addr))return ESP_ERR_INVALID_STATE;
    esp_err_t result=esp_netif_get_dns_info(sta,ESP_NETIF_DNS_MAIN,&dns);
    if(result!=ESP_OK||!dns.ip.u_addr.ip4.addr)return ESP_ERR_INVALID_STATE;
    uint8_t offer=0x02;
    esp_netif_dhcps_stop(ap_netif);
    result=esp_netif_dhcps_option(ap_netif,ESP_NETIF_OP_SET,ESP_NETIF_DOMAIN_NAME_SERVER,&offer,sizeof(offer));
    if(result==ESP_OK)result=esp_netif_set_dns_info(ap_netif,ESP_NETIF_DNS_MAIN,&dns);
    esp_err_t started=esp_netif_dhcps_start(ap_netif);
    if(result==ESP_OK)result=started;
    if(result==ESP_OK)result=esp_netif_set_default_netif(sta);
    if(result==ESP_OK)result=esp_netif_napt_enable(ap_netif);
    if(result==ESP_OK){
        xSemaphoreTake(guard,portMAX_DELAY);state.relay=true;xSemaphoreGive(guard);
        close_at=0;expires=seconds()+600;status("手机重连此热点，用浏览器登录公共网络");
    }
    return result;
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
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
            if(cmd.kind==4||cmd.kind==5){
                test_offline_until=cmd.kind==4?seconds()+300:0;
                if(test_offline_until){esp_wifi_disconnect();xEventGroupClearBits(app_events,READY_BIT);}
                else connect_config(&working);
                continue;
            }
#endif
            if (cmd.kind==1 && !state.active) {
                if (start_setup()!=ESP_OK) status("启动失败，请稍后重试");
            } else if (cmd.kind==2) {
                if(testing) rollback();
                stop_setup();
            } else if(cmd.kind==3 && !testing) {
                if(state.relay){esp_netif_napt_disable(ap_netif);xSemaphoreTake(guard,portMAX_DELAY);state.relay=false;xSemaphoreGive(guard);}
                candidate=cmd.config; testing=true; attempt_deadline=seconds()+25;
                connect_config(&candidate); status("正在连接，请稍候...");
            } else if(cmd.kind==6&&!testing&&!scanning){
                if(connecting&&!wifi_setup_has_ip())esp_wifi_disconnect();
                wifi_scan_config_t scan={.scan_time.active={.min=60,.max=120},.show_hidden=true};
                xSemaphoreTake(guard,portMAX_DELAY);scanning=true;xSemaphoreGive(guard);
                if(esp_wifi_scan_start(&scan,false)!=ESP_OK){xSemaphoreTake(guard,portMAX_DELAY);scanning=false;scan_count=0;scan_revision++;xSemaphoreGive(guard);}
            } else if(cmd.kind==7&&state.active&&!testing){
                if(start_relay()!=ESP_OK)status("辅助认证未开启，请检查上游网络或使用手机热点");
            }
            memset(&cmd,0,sizeof(cmd));
        }
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
        if(test_offline_until){
            if(seconds()>=test_offline_until){test_offline_until=0;connect_config(&working);}
        }
#endif
        EventBits_t bits=xEventGroupClearBits(events,CONNECTED_BIT|LOST_BIT|SCAN_BIT);
        if(bits&SCAN_BIT){
            xSemaphoreTake(guard,portMAX_DELAY);scan_count=WIFI_SETUP_SCAN_LIMIT;
            if(esp_wifi_scan_get_ap_records(&scan_count,scan_rows)!=ESP_OK){scan_count=0;esp_wifi_clear_ap_list();}
            scanning=false;scan_revision++;xSemaphoreGive(guard);
            retry_at=seconds()+2;
        }
        wifi_ap_record_t ap;
        if ((bits&CONNECTED_BIT) && esp_wifi_sta_get_ap_info(&ap)==ESP_OK) {
            connecting=false; failures=0; reconnect_index=0;
            wifi_config_t actual; esp_wifi_get_config(WIFI_IF_STA,&actual);
            if (testing && !memcmp(ap.ssid,candidate.sta.ssid,32)) {
                testing=false; working=actual;
                if(save_profile(&working)==ESP_OK) {
                    status("已连接无线网络；如需网页认证，请用手机辅助");
                    /* DHCP is not proof of Internet access. Keep the helper
                     * available for captive login instead of closing at 8 s. */
                    close_at=0;
                } else status("已连接，但保存失败，请重试");
            } else if(!testing) {
                working=actual;
                if(!profiles.count) save_profile(&working);
            }
        }
        if(testing && seconds()>=attempt_deadline) rollback();
        if((bits&LOST_BIT) && !connecting) { connecting=true; retry_at=seconds()+2; }
        bool reconnect_allowed=true;
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
        reconnect_allowed=!test_offline_until;
#endif
        if(reconnect_allowed && connecting && !scanning && seconds()>=retry_at) {
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
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
    if(nvs_open("wifi_test_once",NVS_READWRITE,&nvs)==ESP_OK){
        uint8_t offline=0;nvs_get_u8(nvs,"offline",&offline);
        nvs_erase_key(nvs,"offline");nvs_commit(nvs);nvs_close(nvs);
        if(offline)test_offline_until=seconds()+300;
    }
#endif
    status("点击手机配网");
    /* The old flash configuration remains untouched as a migration fallback. */
    esp_err_t r=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(r==ESP_OK) r=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,wifi_event,NULL);
    if(r==ESP_OK) r=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL);
    if(r==ESP_OK) r=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,wifi_event,NULL);
    if(r==ESP_OK) r=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,wifi_event,NULL);
    if(r==ESP_OK) r=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_AP_STADISCONNECTED,wifi_event,NULL);
    if(r==ESP_OK) r=esp_wifi_set_mode(WIFI_MODE_STA);
    if(r==ESP_OK && working.sta.ssid[0]) r=esp_wifi_set_config(WIFI_IF_STA,&working);
    if(r==ESP_OK) r=esp_wifi_start();
    if(r!=ESP_OK) return r;
    bool start=true;
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
    start=!test_offline_until;
#endif
    if(start&&working.sta.ssid[0]) { connecting=true; retry_at=seconds()+10; esp_wifi_connect(); }
    return xTaskCreateWithCaps(setup_task,"wifi_setup",4096,NULL,3,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
esp_err_t wifi_setup_open(void)
{
    if(!commands) return ESP_ERR_INVALID_STATE;
    command_t cmd={.kind=1};
    return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}
esp_err_t wifi_setup_portal(void)
{
    wifi_setup_state_t s;wifi_setup_state(&s);
    if(!commands||!s.active||s.testing||!wifi_setup_has_ip())return ESP_ERR_INVALID_STATE;
    command_t cmd={.kind=7};return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}
esp_err_t wifi_setup_scan(void)
{
    if(!commands)return ESP_ERR_INVALID_STATE;
    wifi_setup_state_t s;wifi_setup_state(&s);
    if(s.testing)return ESP_ERR_INVALID_STATE;
    command_t cmd={.kind=6};return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}
bool wifi_setup_scan_read(wifi_ap_record_t *rows,uint16_t *count,uint32_t *revision)
{
    if(!guard)return false;
    xSemaphoreTake(guard,portMAX_DELAY);
    uint16_t n=*count<scan_count?*count:scan_count;
    if(rows)memcpy(rows,scan_rows,n*sizeof(*rows));
    *count=n;*revision=scan_revision;bool busy=scanning;
    xSemaphoreGive(guard);return busy;
}
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
esp_err_t wifi_setup_test_offline_boot(void)
{
    nvs_handle_t h;esp_err_t r=nvs_open("wifi_test_once",NVS_READWRITE,&h);
    if(r!=ESP_OK)return r;
    r=nvs_set_u8(h,"offline",1);if(r==ESP_OK)r=nvs_commit(h);nvs_close(h);return r;
}
bool wifi_setup_test_service_blocked(void){return test_service_blocked;}
void wifi_setup_test_block_service(bool blocked){test_service_blocked=blocked;}
esp_err_t wifi_setup_test_offline(bool offline)
{
    command_t cmd={.kind=offline?4:5};
    return commands&&xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_INVALID_STATE;
}
#endif
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
