#include "device_api.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "service_config.h"
#include <stdio.h>
#include <string.h>
extern const uint8_t ca[] asm("_binary_device_ca_pem_start");
extern const uint8_t cert[] asm("_binary_device_client_pem_start");
extern const uint8_t key[] asm("_binary_device_client_key_start");
esp_err_t device_api(const char *path,const char *json,void *output,size_t capacity,size_t *length)
{
    if(!path || strncmp(path,"/v1/",4) || !output || capacity<2) return ESP_ERR_INVALID_ARG;
    char url[256];if(snprintf(url,sizeof(url),CLOCK_API_BASE "%s",path)>=(int)sizeof(url))return ESP_ERR_INVALID_SIZE;
    esp_http_client_config_t cfg={.url=url,.timeout_ms=15000,.buffer_size=2048,
        .cert_pem=(char *)ca,.client_cert_pem=(char *)cert,.client_key_pem=(char *)key,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);if(!client)return ESP_ERR_NO_MEM;
    if(CLOCK_WALLPAPER_TOKEN[0]) { char auth[192];snprintf(auth,sizeof(auth),"Bearer %s",CLOCK_WALLPAPER_TOKEN);esp_http_client_set_header(client,"Authorization",auth); }
    size_t send=json?strlen(json):0;
    if(json) { esp_http_client_set_method(client,HTTP_METHOD_POST);esp_http_client_set_header(client,"Content-Type","application/json"); }
    esp_err_t result=esp_http_client_open(client,send);
    size_t sent=0,received=0;
    while(result==ESP_OK && sent<send) { int n=esp_http_client_write(client,json+sent,send-sent);if(n<=0)result=ESP_FAIL;else sent+=n; }
    if(result==ESP_OK) {
        int64_t expected=esp_http_client_fetch_headers(client);
        int status=esp_http_client_get_status_code(client);
        if(status!=200 || expected>=(int64_t)capacity)result=status>=400&&status<500?ESP_ERR_INVALID_ARG:ESP_FAIL;
        while(result==ESP_OK && received<capacity-1) {
            int n=esp_http_client_read(client,(char *)output+received,capacity-1-received);
            if(n<0) { result=ESP_FAIL;break; }
            if(!n)break;
            received+=n;
        }
        if(!esp_http_client_is_complete_data_received(client))result=ESP_FAIL;
    }
    ((char *)output)[received]=0;if(length)*length=received;
    if(result!=ESP_OK)ESP_LOGW("device_api","request %s method=%s error=%s status=%d bytes=%u internal=%u",path,json?"POST":"GET",esp_err_to_name(result),esp_http_client_get_status_code(client),(unsigned)received,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_http_client_close(client);esp_http_client_cleanup(client);return result;
}
