#pragma once
#include "lvgl.h"
#include "cJSON.h"
void story_app_create(lv_obj_t *,const lv_font_t *,const lv_font_t *,void (*)(void));
lv_obj_t *story_app_view(void);
void story_app_set_active(bool);
bool story_app_request(const char *id);
void story_app_debug(cJSON *);
