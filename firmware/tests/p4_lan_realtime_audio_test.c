#include <assert.h>
#include <stdio.h>
#include "pet_p4_lan_realtime_crypto.h"

static const uint8_t key[16] = {42};
static const uint8_t binding[16] = {13};
static const uint8_t stale[16] = {12};

static void test_bound_lease(void) {
  pet_lan_rtc_t state = {0};
  uint8_t header[PET_LAN_RTC_HEADER], nonce[12];
  assert(!pet_lan_rtc_activate(&state, 10, binding));
  assert(pet_lan_rtc_offer(&state, 10, binding));
  assert(!pet_lan_rtc_offer(&state, 11, stale));
  assert(!pet_lan_rtc_activate(&state, 11, stale));
  assert(!pet_lan_rtc_next_uplink(&state, 11, binding, PET_LAN_RTC_AUDIO, 0, PET_LAN_RTC_PCM, header, nonce));
  assert(!pet_lan_rtc_activate(&state, 10 + PET_LAN_RTC_START_MS, binding));
  assert(pet_lan_rtc_offer(&state, 21000, binding));
  assert(pet_lan_rtc_activate(&state, 21001, binding));
  assert(!pet_lan_rtc_next_uplink(&state, 21002, stale, PET_LAN_RTC_KEEPALIVE, 0, 0, header, nonce));
  assert(pet_lan_rtc_next_uplink(&state, 21002, binding, PET_LAN_RTC_KEEPALIVE, 0, 0, header, nonce));
  assert(!pet_lan_rtc_live(&state, 21001 + PET_LAN_RTC_IDLE_MS));
  assert(!pet_lan_rtc_activate(&state, 23000, binding));

  assert(pet_lan_rtc_offer(&state, 23000, stale));
  assert(!pet_lan_rtc_activate(&state, 23001, binding));
  assert(pet_lan_rtc_activate(&state, 23002, stale));
  for (uint64_t now = 24000; now < 23002 + PET_LAN_RTC_MAX_MS; now += 1000) {
    pet_lan_rtc_header(header, state.next_in, PET_LAN_RTC_KEEPALIVE, 0, 0);
    assert(pet_lan_rtc_accept_downlink(&state, now, stale, header, 0));
  }
  assert(!pet_lan_rtc_live(&state, 23002 + PET_LAN_RTC_MAX_MS));
}

static void test_authentication_failure(void) {
  pet_lan_rtc_t state = {0};
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  uint8_t wire[PET_LAN_RTC_MAX_RECORD] = {0};
  uint8_t pcm[PET_LAN_RTC_PCM];
  memset(pcm, 0xff, sizeof(pcm));

  assert(pet_lan_rtc_offer(&state, 1, binding));
  pet_lan_rtc_header(wire, 0, PET_LAN_RTC_AUDIO, 0, PET_LAN_RTC_PCM);
  assert(pet_lan_rtc_downlink_size(&state, wire) == sizeof(wire));
  assert(!pet_lan_rtc_open(&gcm, &state, 2, binding, wire, sizeof(wire), pcm));
  assert(pet_lan_rtc_pending(&state, 2));
  assert(pet_lan_rtc_activate(&state, 2, binding));
  assert(!pet_lan_rtc_open(&gcm, &state, 3, binding, wire, sizeof(wire), pcm));
  assert(!pet_lan_rtc_live(&state, 3));
  for (size_t i = 0; i < sizeof(pcm); ++i) assert(!pcm[i]);
  pet_lan_rtc_header(wire, 0, PET_LAN_RTC_AUDIO, 0, PET_LAN_RTC_PCM + 1);
  assert(!pet_lan_rtc_downlink_size(&state, wire));
  mbedtls_gcm_free(&gcm);
}

static void exchange_stdio(void) {
  pet_lan_rtc_t state = {0};
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  assert(!mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128));
  assert(pet_lan_rtc_offer(&state, 1, binding));
  assert(pet_lan_rtc_activate(&state, 2, binding));
  uint8_t wire[PET_LAN_RTC_MAX_RECORD];
  uint8_t pcm[PET_LAN_RTC_PCM];

  assert(fread(wire, 1, sizeof(wire), stdin) == sizeof(wire));
  assert(!pet_lan_rtc_open(&gcm, &state, 3, stale, wire, sizeof(wire), pcm));
  assert(pet_lan_rtc_open(&gcm, &state, 3, binding, wire, sizeof(wire), pcm) == PET_LAN_RTC_AUDIO);
  for (size_t i = 0; i < sizeof(pcm); ++i) assert(pcm[i] == (uint8_t)i);

  size_t size = pet_lan_rtc_seal(&gcm, &state, 4, binding, PET_LAN_RTC_AUDIO,
                                PET_LAN_RTC_VAD_VALID | PET_LAN_RTC_SPEECH, pcm, sizeof(pcm), wire);
  assert(size == sizeof(wire));
  assert(fwrite(wire, 1, size, stdout) == size && !fflush(stdout));

  size = PET_LAN_RTC_HEADER + PET_LAN_RTC_TAG;
  assert(fread(wire, 1, size, stdin) == size);
  assert(pet_lan_rtc_open(&gcm, &state, 5, binding, wire, size, pcm) == PET_LAN_RTC_CLOSE);
  assert(!pet_lan_rtc_next_uplink(&state, 6, binding, PET_LAN_RTC_AUDIO, 0, sizeof(pcm), wire, (uint8_t[12]){0}));
  mbedtls_gcm_free(&gcm);
}

int main(int argc, char **argv) {
  (void)argv;
  test_bound_lease();
  test_authentication_failure();
  if (argc > 1) exchange_stdio();
}
