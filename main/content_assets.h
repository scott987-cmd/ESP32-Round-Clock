#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
esp_err_t content_assets_init(void);
bool content_assets_ready(void);
esp_err_t content_read(uint32_t offset,void *target,size_t bytes);
bool content_picture(unsigned word,unsigned size,uint8_t *target);
esp_err_t content_word_play(unsigned word);
