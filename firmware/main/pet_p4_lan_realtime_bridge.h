#pragma once

#include "pet_p4_lan_realtime_crypto.h"

enum { PET_LAN_RTC_CAPTURE_QUEUE = 4, PET_LAN_RTC_CAPTURE_MAX_AGE_MS = 160 };

/* The audio owner supplies these adapters after it can arbitrate USB, component
 * and LAN operations in one place. A successful start claims capture AND the
 * speaker for this binding. A failed start leaves no reservation or mic work.
 * stop closes capture, flushes the speaker and releases that same claim. */
typedef struct {
  bool (*start)(void *ctx, const uint8_t binding[PET_LAN_RTC_BINDING]);
  void (*stop)(void *ctx, const uint8_t binding[PET_LAN_RTC_BINDING]);
  bool (*play_begin)(void *ctx, uint32_t turn);
  bool (*play_pcm)(void *ctx, const uint8_t pcm[PET_LAN_RTC_PCM]);
  bool (*play_end)(void *ctx);
  void (*play_flush)(void *ctx);
} pet_lan_rtc_bridge_audio_t;

typedef struct {
  uint64_t captured_at;
  uint8_t pcm[PET_LAN_RTC_PCM];
  uint8_t flags;
} pet_lan_rtc_capture_frame_t;

/* All calls are serialized by the same audio owner. The AFE tap must use a
 * try-only entry or an owner event and never wait for codec/network I/O.
 * The caller owns GCM and clears it when this connection is retired. */
typedef struct {
  pet_lan_rtc_t lease;
  uint8_t binding[PET_LAN_RTC_BINDING];
  const pet_lan_rtc_bridge_audio_t *audio;
  void *ctx;
  bool attached;
  bool started;
  bool playing;
  uint32_t turn;
  unsigned capture_head;
  unsigned capture_count;
  pet_lan_rtc_capture_frame_t capture[PET_LAN_RTC_CAPTURE_QUEUE];
} pet_lan_rtc_bridge_t;

static inline bool pet_lan_rtc_bridge_matches(const pet_lan_rtc_bridge_t *bridge,
    const uint8_t binding[PET_LAN_RTC_BINDING]) {
  return bridge->attached && memcmp(bridge->binding, binding, PET_LAN_RTC_BINDING) == 0;
}

/* All termination paths enter here. Revoke routing and remove queued PCM
 * before calling the audio owner's existing stop/quiescence path. */
static inline bool pet_lan_rtc_bridge_stop(pet_lan_rtc_bridge_t *bridge,
    const uint8_t binding[PET_LAN_RTC_BINDING]) {
  if (!pet_lan_rtc_bridge_matches(bridge, binding)) return false;
  const bool started = bridge->started;
  bridge->attached = false;
  bridge->started = false;
  bridge->playing = false;
  bridge->turn = 0;
  bridge->capture_head = 0;
  bridge->capture_count = 0;
  pet_lan_rtc_end(&bridge->lease);
  mbedtls_platform_zeroize(bridge->capture, sizeof(bridge->capture));
  if (started) bridge->audio->stop(bridge->ctx, bridge->binding);
  mbedtls_platform_zeroize(bridge->binding, sizeof(bridge->binding));
  bridge->audio = NULL;
  bridge->ctx = NULL;
  return true;
}

/* Run from an independent owner timer, including during transport backpressure.
 * For socket loss, local cancel and audio worker failure call stop immediately. */
static inline bool pet_lan_rtc_bridge_tick(pet_lan_rtc_bridge_t *bridge, uint64_t now) {
  if (!bridge->attached) return false;
  if (pet_lan_rtc_pending(&bridge->lease, now) || pet_lan_rtc_live(&bridge->lease, now))
    return true;
  pet_lan_rtc_bridge_stop(bridge, bridge->binding);
  return false;
}

typedef enum {
  PET_LAN_RTC_TRUST_UNKNOWN = 0,
  PET_LAN_RTC_TRUST_FIRST_CLAIM,
  PET_LAN_RTC_TRUST_CONTROLLER,
} pet_lan_rtc_trust_t;

typedef enum {
  PET_LAN_RTC_INTENT_NONE = 0,
  PET_LAN_RTC_INTENT_DESKTOP_CHAT_START,
} pet_lan_rtc_intent_t;

/* Internal LAN-owner input. Trust comes from the local pairing record for the
 * authenticated key and generation. Intent comes from a dedicated authenticated
 * Start request issued by the normal desktop chat controls. Never decode a
 * peer-supplied trust flag or infer either value from a keepalive. */
typedef struct {
  uint8_t binding[PET_LAN_RTC_BINDING];
  pet_lan_rtc_trust_t trust;
  pet_lan_rtc_intent_t intent;
} pet_lan_rtc_trusted_session_t;

static inline bool pet_lan_rtc_bridge_offer(pet_lan_rtc_bridge_t *bridge, uint64_t now,
    const pet_lan_rtc_trusted_session_t *session, const pet_lan_rtc_bridge_audio_t *audio, void *ctx) {
  if (bridge->attached || !session || session->trust != PET_LAN_RTC_TRUST_CONTROLLER ||
      session->intent != PET_LAN_RTC_INTENT_DESKTOP_CHAT_START || !audio || !audio->start ||
      !audio->stop || !audio->play_begin || !audio->play_pcm || !audio->play_end || !audio->play_flush)
    return false;
  if (!pet_lan_rtc_offer(&bridge->lease, now, session->binding)) return false;
  memcpy(bridge->binding, session->binding, PET_LAN_RTC_BINDING);
  bridge->audio = audio;
  bridge->ctx = ctx;
  bridge->attached = true;
  return true;
}

/* Call from authenticated, ordered turn control on the same socket. Wire v1
 * currently has no turn-control record, so production needs that negotiation. */
static inline bool pet_lan_rtc_bridge_play_begin(pet_lan_rtc_bridge_t *bridge, uint64_t now,
    const uint8_t binding[PET_LAN_RTC_BINDING], uint32_t turn) {
  if (!pet_lan_rtc_bridge_matches(bridge, binding) || !pet_lan_rtc_bridge_tick(bridge, now) ||
      !bridge->started || turn <= bridge->turn) return false;
  if (bridge->playing) bridge->audio->play_flush(bridge->ctx);
  if (!bridge->audio->play_begin(bridge->ctx, turn)) {
    pet_lan_rtc_bridge_stop(bridge, binding);
    return false;
  }
  bridge->turn = turn;
  bridge->playing = true;
  return true;
}

static inline bool pet_lan_rtc_bridge_play_finish(pet_lan_rtc_bridge_t *bridge, uint64_t now,
    const uint8_t binding[PET_LAN_RTC_BINDING], uint32_t turn, bool flush) {
  if (!pet_lan_rtc_bridge_matches(bridge, binding) || !pet_lan_rtc_bridge_tick(bridge, now) ||
      !bridge->started || !turn || turn != bridge->turn || (!flush && !bridge->playing)) return false;
  if (flush) bridge->audio->play_flush(bridge->ctx);
  else if (!bridge->audio->play_end(bridge->ctx)) {
    pet_lan_rtc_bridge_stop(bridge, binding);
    return false;
  }
  bridge->playing = false;
  return true;
}

/* The first authenticated keepalive commits an admitted desktop Start.
 * Neither the trusted pairing itself nor admission starts the mic. */
static inline bool pet_lan_rtc_bridge_receive(pet_lan_rtc_bridge_t *bridge, uint64_t now,
    const uint8_t binding[PET_LAN_RTC_BINDING], mbedtls_gcm_context *gcm, const uint8_t *wire,
    size_t size) {
  if (!pet_lan_rtc_bridge_matches(bridge, binding) || !pet_lan_rtc_bridge_tick(bridge, now)) return false;
  if (!bridge->started && !pet_lan_rtc_activate(&bridge->lease, now, binding)) {
    pet_lan_rtc_bridge_stop(bridge, binding);
    return false;
  }
  uint8_t pcm[PET_LAN_RTC_PCM];
  const unsigned kind = pet_lan_rtc_open(gcm, &bridge->lease, now, binding, wire, size, pcm);
  bool accepted = false;
  if (kind == PET_LAN_RTC_KEEPALIVE) {
    if (!bridge->started) bridge->started = bridge->audio->start(bridge->ctx, bridge->binding);
    accepted = bridge->started;
  } else if (kind == PET_LAN_RTC_AUDIO && bridge->started && bridge->playing) {
    accepted = bridge->audio->play_pcm(bridge->ctx, pcm);
  }
  mbedtls_platform_zeroize(pcm, sizeof(pcm));
  if (!accepted) pet_lan_rtc_bridge_stop(bridge, binding);
  return accepted || kind == PET_LAN_RTC_CLOSE;
}

/* This is the post-AFE tap. No allocation, I/O, or stop callback on this path.
 * If the network is slow, drop the newest frame and preserve queued ordering. */
static inline bool pet_lan_rtc_bridge_capture(pet_lan_rtc_bridge_t *bridge, uint64_t now,
    const uint8_t binding[PET_LAN_RTC_BINDING], const uint8_t pcm[PET_LAN_RTC_PCM],
    bool processed, bool speech) {
  if (!processed || !pet_lan_rtc_bridge_matches(bridge, binding) || !bridge->started ||
      now >= bridge->lease.idle_until || now >= bridge->lease.hard_until ||
      bridge->capture_count == PET_LAN_RTC_CAPTURE_QUEUE) return false;
  const unsigned slot = (bridge->capture_head + bridge->capture_count) % PET_LAN_RTC_CAPTURE_QUEUE;
  bridge->capture[slot].captured_at = now;
  memcpy(bridge->capture[slot].pcm, pcm, PET_LAN_RTC_PCM);
  bridge->capture[slot].flags = PET_LAN_RTC_VAD_VALID | (speech ? PET_LAN_RTC_SPEECH : 0);
  ++bridge->capture_count;
  return true;
}

static inline void pet_lan_rtc_bridge_pop_capture(pet_lan_rtc_bridge_t *bridge) {
  mbedtls_platform_zeroize(&bridge->capture[bridge->capture_head], sizeof(bridge->capture[0]));
  bridge->capture_head = (bridge->capture_head + 1) % PET_LAN_RTC_CAPTURE_QUEUE;
  --bridge->capture_count;
}

/* Return ciphertext only. Even a failed write retires this connection. */
static inline size_t pet_lan_rtc_bridge_send(pet_lan_rtc_bridge_t *bridge, uint64_t now,
    const uint8_t binding[PET_LAN_RTC_BINDING], mbedtls_gcm_context *gcm,
    uint8_t wire[PET_LAN_RTC_MAX_RECORD]) {
  if (!pet_lan_rtc_bridge_matches(bridge, binding) || !pet_lan_rtc_bridge_tick(bridge, now) ||
      !bridge->started) return 0;
  while (bridge->capture_count &&
         now - bridge->capture[bridge->capture_head].captured_at >= PET_LAN_RTC_CAPTURE_MAX_AGE_MS)
    pet_lan_rtc_bridge_pop_capture(bridge);
  if (!bridge->capture_count) return 0;
  pet_lan_rtc_capture_frame_t *frame = &bridge->capture[bridge->capture_head];
  const size_t size = pet_lan_rtc_seal(gcm, &bridge->lease, now, binding, PET_LAN_RTC_AUDIO,
                                       frame->flags, frame->pcm, sizeof(frame->pcm), wire);
  pet_lan_rtc_bridge_pop_capture(bridge);
  if (!size) pet_lan_rtc_bridge_stop(bridge, binding);
  return size;
}
