#pragma once
#include "lvgl.h"
#include "cJSON.h"
#include "esp_err.h"
/* Producers may run on workers; only tick/create touch LVGL or NVS. */
typedef enum { NOTICE_WORKING, NOTICE_DONE, NOTICE_FAILED } notice_state_t;
esp_err_t notification_post(const char *key, const char *text, unsigned view, notice_state_t state);
void notification_center_create(lv_obj_t *screen, const lv_font_t *body,
                                const lv_font_t *small, const lv_font_t *title, void (*open)(unsigned));
void notification_center_tick(void);
void notification_center_close(void);
void notification_center_debug(cJSON *root);
/* Physical USB only: labelled local fixtures, never model calls or task changes. */
esp_err_t notification_center_test(unsigned phase);
