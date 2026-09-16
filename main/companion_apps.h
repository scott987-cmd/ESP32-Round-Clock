#pragma once
#include "lvgl.h"
#include "cJSON.h"
#include "esp_err.h"
void companion_apps_create(lv_obj_t *screen, const lv_font_t *font, const lv_font_t *title_font,
                           void (*home)(void));
lv_obj_t *companion_apps_view(unsigned app);
void companion_apps_activate(unsigned app);
void companion_apps_tick(bool awake);
esp_err_t companion_apps_start(void);
void companion_apps_debug(cJSON *root);
/* Fixed, explicitly labelled fixture through the real persistence/upload path. USB only. */
esp_err_t companion_apps_test_note(void);
