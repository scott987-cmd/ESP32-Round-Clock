#pragma once
#include "lvgl.h"
#include "cJSON.h"
void kids_apps_create(lv_obj_t *,const lv_font_t *,const lv_font_t *,void (*)(void));
lv_obj_t *kids_apps_view(unsigned index);
void kids_apps_activate(int index);
void kids_apps_debug(cJSON *);
