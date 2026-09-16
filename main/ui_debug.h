#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t ui_debug_show_view(uint8_t view);
esp_err_t ui_debug_start_pairing(void);
esp_err_t ui_debug_scroll_settings(bool to_bottom);
esp_err_t ui_debug_scroll_overview(bool to_bottom);
esp_err_t ui_debug_scroll_desktop(bool to_bottom);
esp_err_t ui_debug_scroll_quota(bool to_bottom);
esp_err_t ui_debug_arm_codex_reset_alert(void);
esp_err_t ui_debug_play_music(void);
esp_err_t ui_debug_pointer(int x, int y, bool pressed);
esp_err_t ui_debug_state(char *buffer, size_t capacity);
