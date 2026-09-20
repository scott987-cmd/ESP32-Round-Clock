#pragma once
#include "esp_wifi.h"
#include "freertos/event_groups.h"

typedef struct {
    bool active;
    bool testing;
    bool relay;
    unsigned clients;
    unsigned saved_count;
    char ssid[33];
    char password[17];
    char status[128];
} wifi_setup_state_t;

esp_err_t wifi_setup_init(EventGroupHandle_t connected_events, const wifi_config_t *initial);
esp_err_t wifi_setup_open(void);
esp_err_t wifi_setup_portal(void);
void wifi_setup_close(void);
esp_err_t wifi_setup_connect(const char *ssid, const char *password);
void wifi_setup_state(wifi_setup_state_t *out);
bool wifi_setup_has_ip(void);
#define WIFI_SETUP_SCAN_LIMIT 24
esp_err_t wifi_setup_scan(void);
bool wifi_setup_scan_read(wifi_ap_record_t *rows,uint16_t *count,uint32_t *revision);
/* USB-only acceptance hook: never exposes credentials. */
esp_err_t wifi_setup_test_connection(bool invalid);
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
esp_err_t wifi_setup_test_offline(bool offline);
esp_err_t wifi_setup_test_offline_boot(void);
bool wifi_setup_test_service_blocked(void);
void wifi_setup_test_block_service(bool blocked);
#endif
