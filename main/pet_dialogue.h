#pragma once
#include "sdkconfig.h"
#include "esp_err.h"
#include <stdint.h>
typedef enum { PET_IDLE, PET_LISTENING, PET_THINKING, PET_READY, PET_FAILED } pet_phase_t;
typedef struct {pet_phase_t phase; unsigned sequence; char reply[128]; int action; unsigned bytes;} pet_dialogue_state_t;
esp_err_t pet_dialogue_start(void);
void pet_dialogue_finish(void);
pet_dialogue_state_t pet_dialogue_state(void);
esp_err_t pet_dialogue_play(void);
void pet_dialogue_leave(void);
#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
esp_err_t pet_dialogue_test_text(const char *text);
#endif
