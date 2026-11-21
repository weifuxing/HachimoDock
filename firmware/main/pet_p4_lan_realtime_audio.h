#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Secure audio record v1. The paired LAN handshake supplies a fresh audio-only
 * key and a binding unique to this connection. The network owner serializes
 * calls and enforces the trusted-session policy before activating a lease. */
enum {
  PET_LAN_RTC_BINDING = 16,
  PET_LAN_RTC_HEADER = 8,
  PET_LAN_RTC_PCM = 640, /* S16LE, 16 kHz mono, 20 ms. */
  PET_LAN_RTC_TAG = 16,
  PET_LAN_RTC_START_MS = 20000,
  PET_LAN_RTC_IDLE_MS = 1500,
  PET_LAN_RTC_MAX_MS = 15 * 60 * 1000,
  PET_LAN_RTC_DOWN = 1,
  PET_LAN_RTC_UP = 2,
  PET_LAN_RTC_SPEECH = 1,
  PET_LAN_RTC_VAD_VALID = 2,
};

typedef enum {
  PET_LAN_RTC_AUDIO = 1,
  PET_LAN_RTC_KEEPALIVE,
  PET_LAN_RTC_CLOSE,
} pet_lan_rtc_kind_t;

typedef struct {
  uint8_t binding[PET_LAN_RTC_BINDING];
  uint64_t start_until;
  uint64_t idle_until;
  uint64_t hard_until;
  uint32_t next_in;
  uint32_t next_out;
} pet_lan_rtc_t;

static inline void pet_lan_rtc_end(pet_lan_rtc_t *state) {
  *state = (pet_lan_rtc_t){0};
}

static inline bool pet_lan_rtc_pending(pet_lan_rtc_t *state, uint64_t now) {
  if (state->start_until && now >= state->start_until) pet_lan_rtc_end(state);
  return state->start_until != 0;
}

static inline bool pet_lan_rtc_live(pet_lan_rtc_t *state, uint64_t now) {
  if (state->hard_until && (now >= state->hard_until || now >= state->idle_until))
    pet_lan_rtc_end(state);
  return state->hard_until != 0;
}

static inline bool pet_lan_rtc_offer(pet_lan_rtc_t *state, uint64_t now,
                                    const uint8_t binding[PET_LAN_RTC_BINDING]) {
  if (pet_lan_rtc_pending(state, now) || pet_lan_rtc_live(state, now)) return false;
  memcpy(state->binding, binding, PET_LAN_RTC_BINDING);
  state->start_until = now + PET_LAN_RTC_START_MS;
  return true;
}

/* Low-level lease transition, callable only after admission by its owner. */
static inline bool pet_lan_rtc_activate(pet_lan_rtc_t *state, uint64_t now,
                                             const uint8_t binding[PET_LAN_RTC_BINDING]) {
  if (!pet_lan_rtc_pending(state, now) || memcmp(state->binding, binding, PET_LAN_RTC_BINDING))
    return false;
  state->start_until = 0;
  state->idle_until = now + PET_LAN_RTC_IDLE_MS;
  state->hard_until = now + PET_LAN_RTC_MAX_MS;
  return true;
}

static inline bool pet_lan_rtc_shape(unsigned direction, unsigned kind, unsigned flags, size_t bytes) {
  if (kind == PET_LAN_RTC_AUDIO) {
    if (bytes != PET_LAN_RTC_PCM) return false;
    return flags == 0 || (direction == PET_LAN_RTC_UP &&
        (flags == PET_LAN_RTC_VAD_VALID || flags == (PET_LAN_RTC_SPEECH | PET_LAN_RTC_VAD_VALID)));
  }
  return (kind == PET_LAN_RTC_KEEPALIVE || kind == PET_LAN_RTC_CLOSE) && !flags && !bytes;
}

static inline void pet_lan_rtc_header(uint8_t h[PET_LAN_RTC_HEADER], uint32_t seq,
                                      unsigned kind, unsigned flags, size_t size) {
  h[0] = kind; h[1] = flags; h[2] = size >> 8; h[3] = size;
  h[4] = seq >> 24; h[5] = seq >> 16; h[6] = seq >> 8; h[7] = seq;
}

static inline void pet_lan_rtc_nonce(uint8_t nonce[12], unsigned direction, uint32_t seq) {
  const uint8_t bytes[12] = {'R', 'T', 'C', 1, direction, 0, 0, 0,
                             seq >> 24, seq >> 16, seq >> 8, seq};
  memcpy(nonce, bytes, sizeof(bytes));
}

/* Once a header is issued its nonce is spent, even if encryption or socket
 * output fails. The caller must end on either failure and must not retry. */
static inline bool pet_lan_rtc_next_uplink(pet_lan_rtc_t *state, uint64_t now,
                                           const uint8_t binding[PET_LAN_RTC_BINDING],
                                           unsigned kind, unsigned flags, size_t bytes,
                                           uint8_t header[PET_LAN_RTC_HEADER], uint8_t nonce[12]) {
  if (!pet_lan_rtc_live(state, now) || memcmp(state->binding, binding, PET_LAN_RTC_BINDING) ||
      state->next_out == UINT32_MAX || !pet_lan_rtc_shape(PET_LAN_RTC_UP, kind, flags, bytes))
    return false;
  pet_lan_rtc_header(header, state->next_out, kind, flags, bytes);
  pet_lan_rtc_nonce(nonce, PET_LAN_RTC_UP, state->next_out++);
  if (kind == PET_LAN_RTC_CLOSE) pet_lan_rtc_end(state);
  return true;
}

/* Size of an unauthenticated candidate record, or zero. Check this after
 * receiving the fixed header and before reading any body bytes. */
static inline size_t pet_lan_rtc_downlink_size(const pet_lan_rtc_t *state,
                                               const uint8_t header[PET_LAN_RTC_HEADER]) {
  const size_t bytes = (size_t)header[2] << 8 | header[3];
  uint8_t expected[PET_LAN_RTC_HEADER];
  pet_lan_rtc_header(expected, state->next_in, header[0], header[1], bytes);
  if (state->next_in == UINT32_MAX || !pet_lan_rtc_shape(PET_LAN_RTC_DOWN, header[0], header[1], bytes) ||
      memcmp(expected, header, sizeof(expected))) return 0;
  return PET_LAN_RTC_HEADER + bytes + PET_LAN_RTC_TAG;
}

/* Call only after AES-GCM validation against the complete header as AAD and
 * the expected next_in nonce. Authentication/framing errors terminate the
 * original connection. A callback with a different binding cannot alter it. */
static inline bool pet_lan_rtc_accept_downlink(pet_lan_rtc_t *state, uint64_t now,
                                                const uint8_t binding[PET_LAN_RTC_BINDING],
                                                const uint8_t header[PET_LAN_RTC_HEADER],
                                                size_t bytes) {
  if (!pet_lan_rtc_live(state, now) || memcmp(state->binding, binding, PET_LAN_RTC_BINDING))
    return false;
  if (bytes > PET_LAN_RTC_PCM ||
      pet_lan_rtc_downlink_size(state, header) != PET_LAN_RTC_HEADER + bytes + PET_LAN_RTC_TAG) {
    pet_lan_rtc_end(state);
    return false;
  }
  ++state->next_in;
  if (header[0] == PET_LAN_RTC_CLOSE) pet_lan_rtc_end(state);
  else state->idle_until = now + PET_LAN_RTC_IDLE_MS;
  return true;
}
