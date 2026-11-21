#pragma once

#if PET_P4_SYSTEM_WIFI
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t pet_p4_audio_music_push_if_owner(const char *session_id, const uint8_t *pcm, size_t length);
bool pet_p4_audio_music_flush_if_owner(const char *session_id);
bool pet_p4_audio_music_end_if_owner(const char *session_id);
bool pet_p4_audio_music_finish_if_owner(const char *session_id, uint32_t *played_ms);
#endif
