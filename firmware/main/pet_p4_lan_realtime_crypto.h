#pragma once

#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include "pet_p4_lan_realtime_audio.h"

#define PET_LAN_RTC_MAX_RECORD (PET_LAN_RTC_HEADER + PET_LAN_RTC_PCM + PET_LAN_RTC_TAG)

/* Initialize GCM with the single-use 128-bit audio key from the paired
 * handshake. Free the context and clear PCM buffers when the socket ends. */
static inline size_t pet_lan_rtc_seal(mbedtls_gcm_context *gcm, pet_lan_rtc_t *state,
    uint64_t now, const uint8_t binding[PET_LAN_RTC_BINDING], unsigned kind, unsigned flags,
    const uint8_t *pcm, size_t bytes, uint8_t wire[PET_LAN_RTC_MAX_RECORD]) {
  uint8_t nonce[12];
  if (!pet_lan_rtc_next_uplink(state, now, binding, kind, flags, bytes, wire, nonce)) return 0;
  uint8_t empty = 0;
  if (mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, bytes, nonce, sizeof(nonce),
      wire, PET_LAN_RTC_HEADER, bytes ? pcm : &empty, wire + PET_LAN_RTC_HEADER,
      PET_LAN_RTC_TAG, wire + PET_LAN_RTC_HEADER + bytes)) {
    pet_lan_rtc_end(state);
    return 0;
  }
  return PET_LAN_RTC_HEADER + bytes + PET_LAN_RTC_TAG;
}

/* Returns the authenticated kind or zero. The record reader must reject frames
 * larger than MAX_RECORD before reading a body and obey the lease deadlines. */
static inline unsigned pet_lan_rtc_open(mbedtls_gcm_context *gcm, pet_lan_rtc_t *state,
    uint64_t now, const uint8_t binding[PET_LAN_RTC_BINDING], const uint8_t *wire, size_t size,
    uint8_t pcm[PET_LAN_RTC_PCM]) {
  mbedtls_platform_zeroize(pcm, PET_LAN_RTC_PCM);
  if (!pet_lan_rtc_live(state, now) || memcmp(state->binding, binding, PET_LAN_RTC_BINDING)) return 0;
  if (size >= PET_LAN_RTC_HEADER + PET_LAN_RTC_TAG && pet_lan_rtc_downlink_size(state, wire) == size) {
    const size_t bytes = size - PET_LAN_RTC_HEADER - PET_LAN_RTC_TAG;
    uint8_t nonce[12];
    pet_lan_rtc_nonce(nonce, PET_LAN_RTC_DOWN, state->next_in);
    if (!mbedtls_gcm_auth_decrypt(gcm, bytes, nonce, sizeof(nonce),
        wire, PET_LAN_RTC_HEADER, wire + PET_LAN_RTC_HEADER + bytes, PET_LAN_RTC_TAG,
        wire + PET_LAN_RTC_HEADER, pcm) &&
        pet_lan_rtc_accept_downlink(state, now, binding, wire, bytes)) return wire[0];
  }
  mbedtls_platform_zeroize(pcm, PET_LAN_RTC_PCM);
  pet_lan_rtc_end(state);
  return 0;
}
