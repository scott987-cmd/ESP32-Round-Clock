#pragma once
#include "lvgl.h"

typedef void (*launcher_open_cb_t)(unsigned index);
void launcher_create(lv_obj_t *parent, const lv_font_t *font,
                     const char *const *names, unsigned count,
                     launcher_open_cb_t open_app, void (*open_overview)(void));
unsigned launcher_selected(void);
bool launcher_animating(void);
void launcher_decorate_icon(lv_obj_t *object, unsigned index);
