#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* One owner for shared I2S initialization, recording, and speaker playback. */
bool audio_bus_try_acquire(void);
void audio_bus_release(void);
esp_err_t audio_bus_open_speaker(void);
esp_err_t audio_bus_write(void *pcm, size_t bytes);
void audio_bus_set_volume(uint8_t volume);
uint8_t audio_bus_volume(void);

/* Caller retains immutable PCM until playback ends (vocabulary uses flash). */
esp_err_t audio_local_play(const uint8_t *pcm,size_t bytes);
esp_err_t audio_local_play_asset(uint32_t offset,size_t bytes);
esp_err_t audio_local_play_file(const char *path,size_t bytes);
void audio_local_stop(void);
typedef struct { bool playing; esp_err_t result; uint32_t sequence; size_t written; } audio_local_state_t;
audio_local_state_t audio_local_state(void);
