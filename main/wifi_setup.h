#pragma once
#include "esp_wifi.h"
#include "freertos/event_groups.h"

typedef struct {
    bool active;
    bool testing;
    unsigned saved_count;
    char ssid[33];
    char password[17];
    char status[128];
} wifi_setup_state_t;

esp_err_t wifi_setup_init(EventGroupHandle_t connected_events, const wifi_config_t *initial);
esp_err_t wifi_setup_open(void);
void wifi_setup_close(void);
esp_err_t wifi_setup_connect(const char *ssid, const char *password);
void wifi_setup_state(wifi_setup_state_t *out);
/* USB-only acceptance hook: never exposes credentials. */
esp_err_t wifi_setup_test_connection(bool invalid);
