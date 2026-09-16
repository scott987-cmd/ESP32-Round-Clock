#pragma once
#include "lvgl.h"
#include "cJSON.h"

void star_game_create(lv_obj_t *screen, const lv_font_t *body, const lv_font_t *title,
                      void (*home)(void));
lv_obj_t *star_game_view(void);
void star_game_set_active(bool active);
void star_game_debug(cJSON *root);
