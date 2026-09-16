#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "nvs.h"

#include "avatar_store.h"
#include "service_config.h"

#define PHOTO_SLOT_AUTO 0xFF

static volatile uint8_t available_mask;
static volatile uint8_t latest_slot = PHOTO_SLOT_AUTO;
static volatile uint32_t revision;
static SemaphoreHandle_t file_lock;
static char hashes[8][65];
static uint8_t active_copy[8];
static const char *sync_status="等待联网同步";
extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");
static void digest(const uint8_t *p,size_t size,char output[65]) {
    unsigned char hash[32]; mbedtls_sha256(p,size,hash,0);
    for(int i=0;i<32;i++) snprintf(output+2*i,3,"%02x",hash[i]);
}

static void avatar_path(char *path, size_t length, uint8_t slot)
{
    snprintf(path, length, "/wallpaper/avatar%u.rgb565", slot);
}

static bool valid_avatar_file(uint8_t slot)
{
    char path[48];
    struct stat info;
    avatar_path(path, sizeof(path), slot);
    return stat(path, &info) == 0 && info.st_size == AVATAR_BYTES;
}

static void load_metadata(void)
{
    for (uint8_t slot = 0; slot < AVATAR_MAX_SLOTS; ++slot) {
        if (valid_avatar_file(slot)) {
            available_mask |= 1U << slot;
        }
    }

    nvs_handle_t handle;
    uint8_t saved_latest = PHOTO_SLOT_AUTO;
    if (nvs_open("avatar", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "latest", &saved_latest);
        nvs_close(handle);
    }
    if (saved_latest < AVATAR_MAX_SLOTS && (available_mask & (1U << saved_latest))) {
        latest_slot = saved_latest;
        return;
    }
    for (uint8_t slot = 0; slot < AVATAR_MAX_SLOTS; ++slot) {
        if (available_mask & (1U << slot)) {
            latest_slot = slot;
            return;
        }
    }
}

esp_err_t avatar_store_init(void)
{
    file_lock=xSemaphoreCreateMutex();
    if(!file_lock) return ESP_ERR_NO_MEM;
    load_metadata();
    uint8_t *data=heap_caps_malloc(AVATAR_PACKAGE_BYTES,MALLOC_CAP_SPIRAM);
    if(!data) return ESP_ERR_NO_MEM;
    for(int i=0;i<8;i++) {
        nvs_handle_t handle; char key[8]; snprintf(key,sizeof(key),"copy%d",i);
        bool committed=false;
        if(nvs_open("avatar",NVS_READONLY,&handle)==ESP_OK) {
            committed=nvs_get_u8(handle,key,&active_copy[i])==ESP_OK && active_copy[i]<2;
            nvs_close(handle);
        }
        if(!committed) continue;
        char path[48]; snprintf(path,sizeof(path),"/wallpaper/avatar%d-%u.rav",i,active_copy[i]);
        FILE *f=fopen(path,"rb"); if(!f) continue;
        size_t n=fread(data,1,AVATAR_PACKAGE_BYTES,f); int extra=fgetc(f); fclose(f);
        if(extra==EOF && avatar_package_valid(data,n)) {
            digest(data,n,hashes[i]); available_mask|=1U<<i;
            if(latest_slot==PHOTO_SLOT_AUTO) latest_slot=i;
        }
    }
    free(data);
    nvs_handle_t h; uint8_t last;
    if(nvs_open("avatar",NVS_READONLY,&h)==ESP_OK) {
        if(nvs_get_u8(h,"latest",&last)==ESP_OK && last<8 && (available_mask&(1U<<last))) latest_slot=last;
        nvs_close(h);
    }
    return ESP_OK;
}

size_t avatar_store_count(void)
{
    size_t count = 0;
    uint8_t mask = available_mask;
    for (uint8_t slot = 0; slot < AVATAR_MAX_SLOTS; ++slot) {
        count += (mask >> slot) & 1U;
    }
    return count;
}

uint8_t avatar_store_latest_slot(void)
{
    return latest_slot;
}

uint32_t avatar_store_revision(void)
{
    return revision;
}

int avatar_store_adjacent_slot(uint8_t current, int direction)
{
    if (available_mask == 0) {
        return -1;
    }
    int step = direction < 0 ? -1 : 1;
    int slot = current < AVATAR_MAX_SLOTS ? current : 0;
    for (int attempt = 0; attempt < AVATAR_MAX_SLOTS; ++attempt) {
        slot = (slot + step + AVATAR_MAX_SLOTS) % AVATAR_MAX_SLOTS;
        if (available_mask & (1U << slot)) {
            return slot;
        }
    }
    return current < AVATAR_MAX_SLOTS ? current : -1;
}

esp_err_t avatar_store_load(uint8_t slot, uint8_t *target, size_t length)
{
    if (target == NULL || length != AVATAR_BYTES || slot >= AVATAR_MAX_SLOTS ||
        (available_mask & (1U << slot)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[48];
    avatar_path(path, sizeof(path), slot);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    size_t read = fread(target, 1, length, file);
    int extra = fgetc(file);
    fclose(file);
    return read == length && extra == EOF ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t avatar_store_load_face(uint8_t slot,uint8_t *target,avatar_region_t regions[3])
{
    if(slot>=8) return ESP_ERR_INVALID_ARG;
    memset(regions,0,3*sizeof(*regions));
    xSemaphoreTake(file_lock,portMAX_DELAY);
    esp_err_t result=ESP_FAIL;
    if(hashes[slot][0]) {
        char path[48]; snprintf(path,sizeof(path),"/wallpaper/avatar%u-%u.rav",slot,active_copy[slot]);
        FILE *f=fopen(path,"rb");
        if(f) {
            uint8_t header[64]; size_t a=fread(header,1,64,f),b=fread(target,1,AVATAR_BYTES,f);
            fclose(f);
            if(a==64 && b==AVATAR_BYTES) { memcpy(regions,header+8,24); result=ESP_OK; }
        }
    } else result=avatar_store_load(slot,target,AVATAR_BYTES);
    xSemaphoreGive(file_lock); return result;
}
static int fetch(const char *path,uint8_t *data,int capacity)
{
    char url[256]; snprintf(url,sizeof(url),CLOCK_API_BASE "%s",path);
    esp_http_client_config_t cfg={.url=url,.timeout_ms=15000,.buffer_size=2048,
        .disable_auto_redirect=true,.cert_pem=(const char *)device_ca_start,
        .client_cert_pem=(const char *)device_client_cert_start,.client_key_pem=(const char *)device_client_key_start};
    esp_http_client_handle_t client=esp_http_client_init(&cfg); if(!client) return -1;
    if(CLOCK_WALLPAPER_TOKEN[0]) {
        char auth[192];
        snprintf(auth,sizeof(auth),"Bearer %s",CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client,"Authorization",auth);
    }
    int count=-1;
    if(esp_http_client_open(client,0)==ESP_OK) {
        int64_t length=esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client)==200 && length>0 && length<=capacity) {
            count=esp_http_client_read_response(client,(char *)data,capacity);
            if(count!=length || !esp_http_client_is_complete_data_received(client)) count=-1;
        }
    }
    esp_http_client_close(client); esp_http_client_cleanup(client); return count;
}
const char *avatar_store_sync_status(void) { return sync_status; }
esp_err_t avatar_store_clear(uint8_t slot,const char *expected_hash_prefix)
{
    if(slot>=8 || strlen(expected_hash_prefix)!=16) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(file_lock,portMAX_DELAY);
    esp_err_t result=ESP_ERR_INVALID_STATE;
    if(hashes[slot][0] && !strncmp(hashes[slot],expected_hash_prefix,16)) {
        nvs_handle_t handle; char key[8]; snprintf(key,sizeof(key),"copy%u",slot);
        if(nvs_open("avatar",NVS_READWRITE,&handle)==ESP_OK) {
            result=nvs_erase_key(handle,key);
            if(result==ESP_OK) result=nvs_commit(handle);
            nvs_close(handle);
        }
        if(result==ESP_OK) {
            hashes[slot][0]=0; available_mask&=~(1U<<slot);
            if(valid_avatar_file(slot)) available_mask|=1U<<slot;
            latest_slot=PHOTO_SLOT_AUTO;
            for(int i=0;i<8;i++) if(available_mask&(1U<<i)) { latest_slot=i; break; }
            revision++;
            // Bytes remain as recoverable inactive copies; only their selector is removed.
        }
    }
    xSemaphoreGive(file_lock); return result;
}
void avatar_store_sync(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *net=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!net || esp_netif_get_ip_info(net,&ip)!=ESP_OK || !ip.ip.addr) { sync_status="离线 · 已存头像可互动"; return; }
    uint8_t manifest[1400]; int n=fetch("/v1/avatars",manifest,sizeof(manifest)-1);
    if(n<=0) { sync_status="同步失败 · 稍后自动重试"; return; }
    manifest[n]=0; cJSON *root=cJSON_Parse((char *)manifest);
    cJSON *items=cJSON_GetObjectItemCaseSensitive(root,"avatars");
    if(!cJSON_IsArray(items) || cJSON_GetArraySize(items)>8) { cJSON_Delete(root); sync_status="头像列表无效"; return; }
    bool ok=true; cJSON *item;
    cJSON_ArrayForEach(item,items) {
        cJSON *s=cJSON_GetObjectItemCaseSensitive(item,"slot"),*h=cJSON_GetObjectItemCaseSensitive(item,"sha256");
        if(!cJSON_IsNumber(s) || s->valuedouble!=s->valueint || s->valueint<0 || s->valueint>=8 || !cJSON_IsString(h) || strlen(h->valuestring)!=64) { ok=false; break; }
        int slot=s->valueint;
        if(!strcmp(hashes[slot],h->valuestring)) continue;
        sync_status="正在同步头像";
        uint8_t *data=heap_caps_malloc(AVATAR_PACKAGE_BYTES,MALLOC_CAP_SPIRAM);
        if(!data) { ok=false; break; }
        char path[64],actual[65]; snprintf(path,sizeof(path),"/v1/avatars?slot=%d",slot);
        n=fetch(path,data,AVATAR_PACKAGE_BYTES);
        if(n!=AVATAR_PACKAGE_BYTES || !avatar_package_valid(data,n)) { free(data); ok=false; break; }
        digest(data,n,actual);
        if(strcmp(actual,h->valuestring)) { free(data); ok=false; break; }
        // SPIFFS rename does not replace an existing file. Write the inactive
        // copy, close it, then atomically commit its selector in NVS.
        uint8_t copy=1-active_copy[slot];
        char temp[48]; snprintf(temp,sizeof(temp),"/wallpaper/avatar%d-%u.rav",slot,copy);
        FILE *f=fopen(temp,"wb"); bool written=false;
        if(f) { written=fwrite(data,1,n,f)==n; if(fflush(f)!=0) written=false; if(fclose(f)!=0) written=false; }
        free(data);
        xSemaphoreTake(file_lock,portMAX_DELAY);
        nvs_handle_t handle; bool committed=false;
        if(written && nvs_open("avatar",NVS_READWRITE,&handle)==ESP_OK) {
            char key[8]; snprintf(key,sizeof(key),"copy%d",slot);
            committed=nvs_set_u8(handle,key,copy)==ESP_OK && nvs_set_u8(handle,"latest",slot)==ESP_OK && nvs_commit(handle)==ESP_OK;
            nvs_close(handle);
        }
        if(committed) {
            active_copy[slot]=copy;
            strcpy(hashes[slot],actual); available_mask|=1U<<slot; latest_slot=slot; revision++;
        } else { ok=false; }
        xSemaphoreGive(file_lock);
        if(!ok) break;
    }
    cJSON_Delete(root); sync_status=ok?(avatar_store_count()?"已同步 · 点脸互动":"相册为空 · 等待上传"):"同步失败 · 旧头像保留";
}
