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
typedef struct {
    bool busy, recording, playing;
    char saved[4][33]; /* latest, sky, aurora, energy */
    uint32_t sequence, written;
} music_input_state_t;
music_input_state_t music_input_state(void);
void music_input_refresh(void);
esp_err_t music_input_start_radio(unsigned station);
esp_err_t music_input_generate_radio(unsigned station);
esp_err_t music_input_play_station(unsigned station);
void music_input_stop(void);
void music_input_set_volume(uint8_t volume);
bool music_input_is_recording(void);
const char *music_input_status(void);
