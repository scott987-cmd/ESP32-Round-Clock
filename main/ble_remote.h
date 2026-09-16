#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*vibe_remote_audio_ack_cb_t)(uint16_t session, uint8_t status);
typedef void (*vibe_remote_photo_packet_cb_t)(const uint8_t *packet, size_t length);

esp_err_t vibe_remote_prepare(void);
esp_err_t vibe_remote_start_pairing(void);
bool vibe_remote_is_ready(void);
const char *vibe_remote_status(void);
void vibe_remote_set_status(const char *status);
void vibe_remote_set_audio_ack_callback(vibe_remote_audio_ack_cb_t callback);
void vibe_remote_set_photo_packet_callback(vibe_remote_photo_packet_cb_t callback);
esp_err_t vibe_remote_send_voice(void);
esp_err_t vibe_remote_send_confirm(void);
esp_err_t vibe_remote_send_undo(void);
esp_err_t vibe_remote_send_paste(void);
esp_err_t vibe_remote_send_audio_packet(const uint8_t *packet, size_t length);
esp_err_t vibe_remote_send_photo_ack(uint16_t session, uint8_t status, uint8_t slot);
