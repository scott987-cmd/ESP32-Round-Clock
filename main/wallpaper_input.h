#pragma once
void wallpaper_input_request_reload(void);

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wallpaper_input_start(void);
esp_err_t wallpaper_input_finish(void);
bool wallpaper_input_take_reload_request(void);
bool wallpaper_input_has_pending_update(void);
void wallpaper_input_mark_cached_available(void);
void wallpaper_input_mark_download_result(bool persisted);
const char *wallpaper_input_status(void);
