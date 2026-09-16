#pragma once
#include "lvgl.h"
#include "cJSON.h"
void english_app_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void));
lv_obj_t *english_app_view(void);
void english_app_set_active(bool active);
void english_app_debug(cJSON *root);
