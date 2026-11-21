/* Include from pet_p4_audio.c after the stream and music functions. These
 * helpers serialize ownership verification with producer writes and voice
 * takeover, using the audio owner's existing recursive stream lock. */
#if PET_P4_SYSTEM_WIFI
#include "pet_p4_radio_audio.h"

static bool radio_music_owner_locked(const char *session_id) {
  return session_id && session_id[0] && atomic_load(&g_music) &&
         atomic_load(&g_stream_active) && !strcmp(session_id, g_stream_session);
}

esp_err_t pet_p4_audio_music_push_if_owner(const char *session_id, const uint8_t *pcm, size_t length) {
  if (!g_stream_lock) return ESP_ERR_INVALID_STATE;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  esp_err_t result = radio_music_owner_locked(session_id)
    ? pet_p4_audio_stream_push(pcm, length) : ESP_ERR_INVALID_STATE;
  xSemaphoreGiveRecursive(g_stream_lock);
  return result;
}

bool pet_p4_audio_music_flush_if_owner(const char *session_id) {
  if (!g_stream_lock) return false;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  bool owned = radio_music_owner_locked(session_id);
  if (owned) pet_p4_audio_stream_flush();
  xSemaphoreGiveRecursive(g_stream_lock);
  return owned;
}

bool pet_p4_audio_music_end_if_owner(const char *session_id) {
  if (!g_stream_lock) return false;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  bool ended = radio_music_owner_locked(session_id) && pet_p4_audio_stream_end() == ESP_OK;
  xSemaphoreGiveRecursive(g_stream_lock);
  return ended;
}

bool pet_p4_audio_music_finish_if_owner(const char *session_id, uint32_t *played_ms) {
  if (!g_stream_lock) return false;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  bool finished = pet_p4_audio_music_ended() && !strcmp(session_id, g_stream_session);
  if (finished) {
    *played_ms = pet_p4_audio_stream_played_bytes() / 96;
    pet_p4_audio_stream_flush();
  }
  xSemaphoreGiveRecursive(g_stream_lock);
  return finished;
}
#endif
