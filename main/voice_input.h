#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*voice_input_transcript_cb_t)(const char *text, void *context);

esp_err_t voice_input_start(void);
esp_err_t voice_input_start_with_callback(voice_input_transcript_cb_t callback,
                                          void *context);
esp_err_t voice_input_finish(void);
bool voice_input_is_recording(void);
uint8_t voice_input_level(void);
const char *voice_input_status(void);
