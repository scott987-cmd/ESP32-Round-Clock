#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "avatar_face.h"

#define AVATAR_WIDTH 280
#define AVATAR_HEIGHT 280
#define AVATAR_BYTES (AVATAR_WIDTH * AVATAR_HEIGHT * 2)
#define AVATAR_MAX_SLOTS 8

esp_err_t avatar_store_init(void);
size_t avatar_store_count(void);
uint8_t avatar_store_latest_slot(void);
uint32_t avatar_store_revision(void);
int avatar_store_adjacent_slot(uint8_t current, int direction);
esp_err_t avatar_store_load(uint8_t slot, uint8_t *target, size_t length);
esp_err_t avatar_store_load_face(uint8_t slot, uint8_t *target, avatar_region_t regions[3]);
void avatar_store_sync(void);
const char *avatar_store_sync_status(void);
esp_err_t avatar_store_clear(uint8_t slot,const char *expected_hash_prefix);
