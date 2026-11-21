#include <assert.h>
#include "pet_p4_lan_realtime_bridge.h"

static const uint8_t owner[PET_LAN_RTC_BINDING] = {1};
static const uint8_t other[PET_LAN_RTC_BINDING] = {2};
static const uint8_t sample[PET_LAN_RTC_PCM] = {5};
static pet_lan_rtc_bridge_t bridge;
static struct {
  unsigned starts, stops, begins, pushes, ends, flushes;
  bool active, refuse;
} audio;

static bool start(void *ctx, const uint8_t *binding) {
  assert(ctx == &audio && pet_lan_rtc_bridge_matches(&bridge, binding) && !audio.active);
  ++audio.starts;
  return (audio.active = !audio.refuse);
}

static void stop(void *ctx, const uint8_t *binding) {
  assert(ctx == &audio && audio.active && !pet_lan_rtc_bridge_matches(&bridge, binding));
  assert(!bridge.capture_count);
  for (size_t i = 0; i < sizeof(bridge.capture); ++i)
    assert(!((const uint8_t *)bridge.capture)[i]);
  ++audio.stops;
  audio.active = false;
}

static bool begin(void *ctx, uint32_t turn) {
  assert(ctx == &audio && audio.active && turn);
  ++audio.begins;
  return true;
}

static bool push(void *ctx, const uint8_t *pcm) {
  assert(ctx == &audio && audio.active && !memcmp(sample, pcm, sizeof(sample)));
  ++audio.pushes;
  return !audio.refuse;
}

static bool end(void *ctx) {
  assert(ctx == &audio && audio.active);
  ++audio.ends;
  return true;
}

static void flush(void *ctx) {
  assert(ctx == &audio && audio.active);
  ++audio.flushes;
}

static const pet_lan_rtc_bridge_audio_t callbacks = {start, stop, begin, push, end, flush};

static size_t downlink(mbedtls_gcm_context *gcm, uint32_t seq, unsigned kind,
                       uint8_t wire[PET_LAN_RTC_MAX_RECORD]) {
  const size_t len = kind == PET_LAN_RTC_AUDIO ? sizeof(sample) : 0;
  uint8_t nonce[12];
  pet_lan_rtc_header(wire, seq, kind, 0, len);
  pet_lan_rtc_nonce(nonce, PET_LAN_RTC_DOWN, seq);
  assert(!mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, sizeof(nonce),
      wire, PET_LAN_RTC_HEADER, sample, wire + PET_LAN_RTC_HEADER,
      PET_LAN_RTC_TAG, wire + PET_LAN_RTC_HEADER + len));
  return PET_LAN_RTC_HEADER + len + PET_LAN_RTC_TAG;
}

int main(void) {
  mbedtls_gcm_context gcm;
  uint8_t key[16] = {9};
  uint8_t wire[PET_LAN_RTC_MAX_RECORD];
  mbedtls_gcm_init(&gcm);
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  size_t hello = downlink(&gcm, 0, PET_LAN_RTC_KEEPALIVE, wire);
  pet_lan_rtc_trusted_session_t session = {
    .binding = {1}, .trust = PET_LAN_RTC_TRUST_FIRST_CLAIM,
    .intent = PET_LAN_RTC_INTENT_DESKTOP_CHAT_START,
  };
  assert(!pet_lan_rtc_bridge_offer(&bridge, 10, &session, &callbacks, &audio));
  assert(!pet_lan_rtc_bridge_receive(&bridge, 11, owner, &gcm, wire, hello));
  session.trust = PET_LAN_RTC_TRUST_UNKNOWN;
  assert(!pet_lan_rtc_bridge_offer(&bridge, 12, &session, &callbacks, &audio));
  session.trust = PET_LAN_RTC_TRUST_CONTROLLER;
  session.intent = PET_LAN_RTC_INTENT_NONE;
  assert(!pet_lan_rtc_bridge_offer(&bridge, 13, &session, &callbacks, &audio));
  assert(!audio.starts && !audio.stops && !bridge.attached);

  session.intent = PET_LAN_RTC_INTENT_DESKTOP_CHAT_START;
  memcpy(session.binding, other, sizeof(session.binding));
  assert(pet_lan_rtc_bridge_offer(&bridge, 20, &session, &callbacks, &audio));
  assert(!audio.starts && !pet_lan_rtc_bridge_play_begin(&bridge, 22, other, 1));
  assert(!pet_lan_rtc_bridge_capture(&bridge, 22, other, sample, true, false));
  assert(!pet_lan_rtc_bridge_receive(&bridge, 22, owner, &gcm, wire, hello));
  assert(pet_lan_rtc_bridge_receive(&bridge, 22, other, &gcm, wire, hello));
  assert(audio.starts == 1 && audio.active);
  assert(!pet_lan_rtc_bridge_stop(&bridge, owner));
  assert(!pet_lan_rtc_bridge_capture(&bridge, 23, other, sample, false, false));
  for (unsigned i = 0; i < PET_LAN_RTC_CAPTURE_QUEUE; ++i)
    assert(pet_lan_rtc_bridge_capture(&bridge, 23, other, sample, true, i == 0));
  assert(!pet_lan_rtc_bridge_capture(&bridge, 23, other, sample, true, true));
  assert(pet_lan_rtc_bridge_send(&bridge, 24, other, &gcm, wire) == sizeof(wire));
  assert(wire[1] == (PET_LAN_RTC_VAD_VALID | PET_LAN_RTC_SPEECH));
  assert(!pet_lan_rtc_bridge_send(&bridge, 183, other, &gcm, wire));
  assert(!bridge.capture_count && bridge.lease.next_out == 1);

  assert(pet_lan_rtc_bridge_play_begin(&bridge, 184, other, 1));
  size_t media = downlink(&gcm, 1, PET_LAN_RTC_AUDIO, wire);
  assert(pet_lan_rtc_bridge_receive(&bridge, 184, other, &gcm, wire, media));
  assert(pet_lan_rtc_bridge_play_begin(&bridge, 185, other, 2) && audio.flushes == 1);
  assert(!pet_lan_rtc_bridge_play_finish(&bridge, 186, other, 1, true));
  assert(pet_lan_rtc_bridge_play_finish(&bridge, 186, other, 2, false));
  assert(audio.ends == 1 && audio.pushes == 1);
  assert(pet_lan_rtc_bridge_capture(&bridge, 187, other, sample, true, true));
  assert(!pet_lan_rtc_bridge_tick(&bridge, 184 + PET_LAN_RTC_IDLE_MS));
  assert(audio.stops == 1 && !audio.active);
  assert(!pet_lan_rtc_bridge_stop(&bridge, other));

  memcpy(session.binding, owner, sizeof(session.binding));
  assert(pet_lan_rtc_bridge_offer(&bridge, 2000, &session, &callbacks, &audio));
  ++key[0];
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  hello = downlink(&gcm, 0, PET_LAN_RTC_KEEPALIVE, wire);
  assert(pet_lan_rtc_bridge_receive(&bridge, 2002, owner, &gcm, wire, hello));
  assert(pet_lan_rtc_bridge_play_begin(&bridge, 2003, owner, 1));
  assert(pet_lan_rtc_bridge_capture(&bridge, 2003, owner, sample, true, true));
  assert(pet_lan_rtc_bridge_stop(&bridge, owner)); /* Socket loss. */
  assert(audio.stops == 2 && !audio.active);

  memcpy(session.binding, other, sizeof(session.binding));
  assert(pet_lan_rtc_bridge_offer(&bridge, 2010, &session, &callbacks, &audio));
  ++key[0];
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  hello = downlink(&gcm, 0, PET_LAN_RTC_KEEPALIVE, wire);
  audio.refuse = true;
  assert(!pet_lan_rtc_bridge_receive(&bridge, 2012, other, &gcm, wire, hello));
  assert(audio.starts == 3 && audio.stops == 2 && !bridge.attached);
  audio.refuse = false;
  ++key[0];
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  hello = downlink(&gcm, 0, PET_LAN_RTC_KEEPALIVE, wire);
  memcpy(session.binding, owner, sizeof(session.binding));
  assert(pet_lan_rtc_bridge_offer(&bridge, 2020, &session, &callbacks, &audio));
  assert(pet_lan_rtc_bridge_receive(&bridge, 2022, owner, &gcm, wire, hello));
  assert(pet_lan_rtc_bridge_play_begin(&bridge, 2023, owner, 1));
  media = downlink(&gcm, 1, PET_LAN_RTC_AUDIO, wire);
  wire[media - 1] ^= 1;
  assert(!pet_lan_rtc_bridge_receive(&bridge, 2024, owner, &gcm, wire, media));
  assert(audio.stops == 3 && audio.pushes == 1);

  ++key[0];
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  hello = downlink(&gcm, 0, PET_LAN_RTC_KEEPALIVE, wire);
  memcpy(session.binding, other, sizeof(session.binding));
  assert(pet_lan_rtc_bridge_offer(&bridge, 2030, &session, &callbacks, &audio));
  assert(pet_lan_rtc_bridge_receive(&bridge, 2032, other, &gcm, wire, hello));
  const size_t close = downlink(&gcm, 1, PET_LAN_RTC_CLOSE, wire);
  assert(pet_lan_rtc_bridge_receive(&bridge, 2033, other, &gcm, wire, close));
  assert(audio.stops == 4 && !bridge.attached);
  assert(pet_lan_rtc_bridge_offer(&bridge, 2040, &session, &callbacks, &audio));
  assert(!pet_lan_rtc_bridge_receive(&bridge, 2040 + PET_LAN_RTC_START_MS, other, &gcm, wire, close));
  assert(audio.starts == 5 && !bridge.attached);
  ++key[0];
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  hello = downlink(&gcm, 0, PET_LAN_RTC_KEEPALIVE, wire);
  wire[hello - 1] ^= 1;
  assert(pet_lan_rtc_bridge_offer(&bridge, 30000, &session, &callbacks, &audio));
  assert(!pet_lan_rtc_bridge_receive(&bridge, 30001, other, &gcm, wire, hello));
  assert(audio.starts == 5 && !bridge.attached);
  mbedtls_gcm_free(&gcm);
}
