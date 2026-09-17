#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_focus.h"

/* One owner for shared I2S initialization, recording, and speaker playback. */
void audio_bus_init(void);
/* Latest explicit audio action wins. Request never blocks the UI. */
audio_lease_t audio_bus_request(void);
bool audio_bus_is_current(audio_lease_t lease);
void audio_bus_release(audio_lease_t lease);
/* Atomically finish only if still owner (recording commit/cancel boundary). */
bool audio_bus_finish(audio_lease_t lease);
/* Short codec operations only; never hold this lock across network requests. */
esp_err_t audio_bus_lock(audio_lease_t lease);
void audio_bus_unlock(void);
esp_err_t audio_bus_write(audio_lease_t lease, void *pcm, size_t bytes);
void audio_bus_set_volume(uint8_t volume);
uint8_t audio_bus_volume(void);

/* Raw PCM is copied to PSRAM; vocabulary is streamed from flash. */
esp_err_t audio_local_play(const uint8_t *pcm,size_t bytes);
esp_err_t audio_local_play_asset(uint32_t offset,size_t bytes);
esp_err_t audio_local_play_file(const char *path,size_t bytes);
void audio_local_stop(uint32_t sequence);
typedef struct { bool playing; esp_err_t result; uint32_t sequence; size_t written; } audio_local_state_t;
audio_local_state_t audio_local_state(void);
