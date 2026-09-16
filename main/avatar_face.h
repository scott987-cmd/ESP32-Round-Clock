#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AVATAR_PACKAGE_BYTES (64 + 280*280*2)
typedef struct { uint16_t x,y,w,h; } avatar_region_t;
bool avatar_package_valid(const uint8_t *data, size_t size);
void avatar_face_render(const uint16_t *source, uint16_t *target,
                        const avatar_region_t regions[3], int mood, float amount);
