#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t music_input_start(void);
/* Adds a small radio-program context before the spoken music idea. */
esp_err_t music_input_start_with_prefix(const char *prefix);
esp_err_t music_input_finish(void);
esp_err_t music_input_play(void);
esp_err_t music_input_play_artwork(const char *id);
void music_input_stop(void);
void music_input_set_volume(uint8_t volume);
bool music_input_is_recording(void);
const char *music_input_status(void);
