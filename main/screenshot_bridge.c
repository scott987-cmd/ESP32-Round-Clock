#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "screenshot_bridge.h"
#include "ui_debug.h"
#include "wifi_setup.h"
#include "companion_apps.h"
#include "avatar_store.h"
#include "notification_center.h"

#define SCREEN_WIDTH 466
#define SCREEN_HEIGHT 466
#define SCREEN_STRIDE (SCREEN_WIDTH * 2)
#define SCREEN_BYTES (SCREEN_STRIDE * SCREEN_HEIGHT)
#define HEADER_BYTES 20

static const char *TAG = "screenshot";

static void write_u16(uint8_t *target, uint16_t value)
{
    target[0] = value & 0xFF;
    target[1] = value >> 8;
}

static void write_u32(uint8_t *target, uint32_t value)
{
    target[0] = value & 0xFF;
    target[1] = (value >> 8) & 0xFF;
    target[2] = (value >> 16) & 0xFF;
    target[3] = value >> 24;
}

static uint32_t fnv1a(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ data[i]) * 16777619U;
    }
    return hash;
}

static bool write_all(const uint8_t *data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        size_t chunk = length - written;
        if (chunk > 4096) {
            chunk = 4096;
        }
        int count = usb_serial_jtag_write_bytes(data + written, chunk,
                                                pdMS_TO_TICKS(2000));
        if (count <= 0) {
            return false;
        }
        written += count;
    }
    return true;
}

static void send_error(void)
{
    static const uint8_t error[] = {'R', 'S', 'E', '1'};
    write_all(error, sizeof(error));
}

static void capture_and_send(void)
{
    uint8_t *pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, SCREEN_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        send_error();
        return;
    }

    lv_draw_buf_t draw_buffer;
    bool captured = false;
    if (bsp_display_lock(5000) == ESP_OK) {
        if (lv_draw_buf_init(&draw_buffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                             LV_COLOR_FORMAT_RGB565, SCREEN_STRIDE, pixels,
                             SCREEN_BYTES) == LV_RESULT_OK) {
            captured = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                     LV_COLOR_FORMAT_RGB565,
                                                     &draw_buffer) == LV_RESULT_OK;
            if (captured) {
                lv_draw_wait_for_finish();
            }
        }
        bsp_display_unlock();
    }

    if (!captured) {
        heap_caps_free(pixels);
        send_error();
        return;
    }

    uint8_t header[HEADER_BYTES] = {'R', 'S', 'C', '1'};
    write_u16(header + 4, SCREEN_WIDTH);
    write_u16(header + 6, SCREEN_HEIGHT);
    write_u32(header + 8, SCREEN_STRIDE);
    write_u32(header + 12, SCREEN_BYTES);
    write_u32(header + 16, fnv1a(pixels, SCREEN_BYTES));

    bool sent = write_all(header, sizeof(header)) && write_all(pixels, SCREEN_BYTES);
    if (sent) {
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "Screenshot %s", sent ? "sent" : "failed");
    heap_caps_free(pixels);
}

static void screenshot_task(void *context)
{
    (void)context;
    char command[64] = {0};
    size_t length = 0;
    for (;;) {
        char byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (byte == '\n' || byte == '\r') {
            command[length] = '\0';
            if (!strncmp(command,"RCAVCLEAR ",10)) {
                unsigned slot; char hash[17];
                bool ok=sscanf(command+10,"%u %16s",&slot,hash)==2 && slot<8 && avatar_store_clear(slot,hash)==ESP_OK;
                write_all((const uint8_t *)(ok?"RCOK":"RCER"),4);
            }
            else if (!strncmp(command,"RCNOTICE ",9)) {
                unsigned phase; esp_err_t r=ESP_ERR_INVALID_ARG;
                if(sscanf(command+9,"%u",&phase)==1 && bsp_display_lock(5000)==ESP_OK) {
                    r=notification_center_test(phase); bsp_display_unlock();
                }
                write_all((const uint8_t *)(r==ESP_OK?"RCOK":"RCER"),4);
            }
            else if (!strcmp(command,"RCLIBRARY")) {
                esp_err_t result=ui_debug_show_view(18);write_all((const uint8_t *)(result==ESP_OK?"RCOK":"RCER"),4);
            }
            else if (!strcmp(command,"RCPET")||!strcmp(command,"RCSTORY")||!strcmp(command,"RCFLIP")) {
                esp_err_t result=ui_debug_show_view(!strcmp(command,"RCPET")?19:!strcmp(command,"RCSTORY")?20:21);write_all((const uint8_t *)(result==ESP_OK?"RCOK":"RCER"),4);
            }
            else if (!strcmp(command,"RCNOTETEST")) {
                esp_err_t result=companion_apps_test_note();
                write_all((const uint8_t *)(result==ESP_OK?"RCOK":"RCER"),4);
            }
            else if (!strcmp(command,"RCWIFIFAIL") || !strcmp(command,"RCWIFIJOIN")) {
                esp_err_t result=wifi_setup_test_connection(!strcmp(command,"RCWIFIFAIL"));
                write_all((const uint8_t *)(result==ESP_OK?"RCOK":"RCER"),4);
            }
            else if (strncmp(command, "RCPTR ", 6) == 0) {
                int x, y, pressed;
                bool ok = sscanf(command + 6, "%d %d %d", &x, &y, &pressed) == 3 &&
                          (pressed == 0 || pressed == 1) &&
                          ui_debug_pointer(x, y, pressed) == ESP_OK;
                write_all((const uint8_t *)(ok ? "RCOK" : "RCER"), 4);
            }
            else if (strcmp(command, "RCSTATE") == 0) {
                char *state = heap_caps_malloc(12288, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (state != NULL && ui_debug_state(state, 12288) == ESP_OK) {
                    write_all((const uint8_t *)"RCJSON", 6);
                    write_all((const uint8_t *)state, strlen(state));
                    write_all((const uint8_t *)"\n", 1);
                } else send_error();
                heap_caps_free(state);
            }
            else if (strcmp(command, "RCSHOT") == 0) {
                capture_and_send();
            }
            else if (length == 7 && memcmp(command, "RCVIEW", 6) == 0 &&
                     command[6] >= '0' && command[6] <= '8') {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_show_view(command[6] - '0');
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCQUOTA") == 0 ||
                     strcmp(command, "RCRESET") == 0 ||
                     strcmp(command, "RCMUSIC") == 0 ||
                     strcmp(command, "RCRADIO") == 0 ||
                     strcmp(command, "RCWALL") == 0 ||
                     strcmp(command, "RCAPPS") == 0 || strcmp(command,"RCAGENTS")==0 || strcmp(command,"RCNOTES")==0 || strcmp(command,"RCSTARS")==0 || strcmp(command,"RCENGLISH")==0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                uint8_t view = strcmp(command, "RCQUOTA") == 0 ? 8 :
                               strcmp(command, "RCRESET") == 0 ? 9 :
                               strcmp(command, "RCMUSIC") == 0 ? 10 :
                               strcmp(command, "RCRADIO") == 0 ? 11 :
                               strcmp(command, "RCWALL") == 0 ? 12 : strcmp(command,"RCAGENTS")==0 ? 14 : strcmp(command,"RCNOTES")==0 ? 15 : strcmp(command,"RCSTARS")==0 ? 16 : strcmp(command,"RCENGLISH")==0 ? 17 : 13;
                esp_err_t result = ui_debug_show_view(view);
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCPAIR") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_start_pairing();
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCSBOT") == 0 ||
                     strcmp(command, "RCSTOP") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_scroll_settings(command[3] == 'B');
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCABOT") == 0 ||
                     strcmp(command, "RCATOP") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_scroll_overview(command[3] == 'B');
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCDBOT") == 0 ||
                     strcmp(command, "RCDTOP") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_scroll_desktop(command[3] == 'B');
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCQBOT") == 0 ||
                     strcmp(command, "RCQTOP") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_scroll_quota(command[3] == 'B');
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCALERT") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_arm_codex_reset_alert();
                write_all(result == ESP_OK ? ok : error, 4);
            }
            else if (strcmp(command, "RCMPLAY") == 0) {
                static const uint8_t ok[] = {'R', 'C', 'O', 'K'};
                static const uint8_t error[] = {'R', 'C', 'E', 'R'};
                esp_err_t result = ui_debug_play_music();
                write_all(result == ESP_OK ? ok : error, 4);
            }
            length = 0;
        } else if (length + 1 < sizeof(command)) {
            command[length++] = byte;
        } else {
            length = 0;
        }
    }
}

esp_err_t screenshot_bridge_start(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 8192,
        .rx_buffer_size = 256,
    };
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK) {
        return result;
    }
    return xTaskCreatePinnedToCoreWithCaps(screenshot_task, "screenshot", 16384, NULL, 3,
                                   NULL, 1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
