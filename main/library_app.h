#pragma once
#include "lvgl.h"
#include "cJSON.h"
void library_app_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void),void (*open_story)(void));
lv_obj_t *library_app_view(void);
void library_app_set_active(bool enabled);
void library_app_debug(cJSON *root);
