/* Mode 13, included after the paired response and trust helpers in LAN. */
#include "pet_p4_audio.h"
#include "pet_p4_lan_realtime_audio.h"
#include "pet_p4_lan_hud.h"
#include "esp_attr.h"
#include "lwip/tcp.h"

enum {
  LAN_RTC_MODE = 13,
  LAN_RTC_BOOT_BYTES = 16,
  LAN_RTC_BEGIN = 4,
  LAN_RTC_END = 5,
  LAN_RTC_FLUSH = 6,
  LAN_RTC_PTT_READY = 8,
  LAN_RTC_TURN_BYTES = 4,
  LAN_RTC_BODY_MAX = PET_LAN_RTC_PCM + LAN_RTC_TURN_BYTES,
  LAN_RTC_WIRE_MAX = PET_LAN_RTC_HEADER + LAN_RTC_BODY_MAX + LAN_TAG_SIZE,
  LAN_RTC_CAPTURE_BATCH = 2,
  LAN_RTC_CAPTURE_SLOTS = 8,
  LAN_RTC_CAPTURE_BATCH_WAIT_MS = 22,
  LAN_RTC_FRAME_US = 1200000,
  LAN_RTC_WRITE_US = 1500000,
  LAN_RTC_CAPTURE_MAX_AGE_MS = 160,
};

typedef struct {
  uint64_t captured;
  uint8_t pcm[PET_LAN_RTC_PCM];
  bool speech;
} lan_rtc_capture_t;

/* LAN status reads this directly. Includes startup and audio teardown. */
/* RTC ownership is exclusive with a live wired desktop. OTA/component trust
 * intentionally retains its existing policy. Check USB after the pairing lock. */
static bool rtc_wire_trusted(uint32_t pair_generation) {
  return ota_wire_trusted(pair_generation) && !atomic_load(&usb_client_active);
}

static atomic_bool rtc_wire_busy;
static EXT_RAM_BSS_ATTR pet_lan_hud_t rtc_hud;
static portMUX_TYPE rtc_hud_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t rtc_hud_generation;

/* Called by LAN process while the main state lock is held, even without a
 * status snapshot. Never wait for the socket/audio worker in the UI lane. */
static void rtc_hud_process(pet_p4_runtime_state_t *state, uint64_t now, bool allowed, uint32_t pair_generation) {
  portENTER_CRITICAL(&rtc_hud_mux);
  pet_lan_hud_apply(&rtc_hud, state, now, !allowed || pair_generation != rtc_hud_generation);
  portEXIT_CRITICAL(&rtc_hud_mux);
}

static bool rtc_hud_offer(const uint8_t *body) {
  portENTER_CRITICAL(&rtc_hud_mux);
  const bool ok = pet_lan_hud_offer(&rtc_hud, body);
  portEXIT_CRITICAL(&rtc_hud_mux);
  return ok;
}
static atomic_uint rtc_wire_last_exit;
static atomic_bool rtc_wire_last_suspended_power_save;
static struct {
  int fd;
  uint32_t generation, ptt;
  atomic_bool ptt_overrun;
  uint8_t handshake_key[LAN_KEY_SIZE];
  uint8_t media_key[LAN_KEY_SIZE];
  SemaphoreHandle_t capture_lock;
  unsigned head, count;
  lan_rtc_capture_t queue[LAN_RTC_CAPTURE_SLOTS];
} rtc_wire;

static void rtc_wire_tap(void *ctx, pet_p4_audio_wifi_session_t session, uint64_t now,
                        const uint8_t pcm[PET_LAN_RTC_PCM], bool speech) {
  (void)ctx;
  (void)session;
  if (xSemaphoreTake(rtc_wire.capture_lock, rtc_wire.ptt ? 1 : 0) != pdTRUE) {
    if (rtc_wire.ptt) atomic_store(&rtc_wire.ptt_overrun, true);
    return;
  }
  if (rtc_wire.count < LAN_RTC_CAPTURE_SLOTS) {
    const unsigned slot = (rtc_wire.head + rtc_wire.count) % LAN_RTC_CAPTURE_SLOTS;
    rtc_wire.queue[slot].captured = now;
    rtc_wire.queue[slot].speech = speech;
    memcpy(rtc_wire.queue[slot].pcm, pcm, PET_LAN_RTC_PCM);
    ++rtc_wire.count;
  } else if (rtc_wire.ptt) {
    atomic_store(&rtc_wire.ptt_overrun, true);
  }
  xSemaphoreGive(rtc_wire.capture_lock);
}

static unsigned rtc_wire_take(uint8_t pcm[PET_LAN_RTC_PCM], bool wait_for_batch) {
  unsigned flags = 0;
  xSemaphoreTake(rtc_wire.capture_lock, portMAX_DELAY);
  const uint64_t now = clock_ms();
  while (rtc_wire.count && !flags) {
    lan_rtc_capture_t *frame = &rtc_wire.queue[rtc_wire.head];
    const uint64_t age = now - frame->captured;
    if (wait_for_batch && rtc_wire.count < LAN_RTC_CAPTURE_BATCH &&
        age < LAN_RTC_CAPTURE_BATCH_WAIT_MS) break;
    if (age < LAN_RTC_CAPTURE_MAX_AGE_MS) {
      flags = PET_LAN_RTC_VAD_VALID | (frame->speech ? PET_LAN_RTC_SPEECH : 0);
      memcpy(pcm, frame->pcm, PET_LAN_RTC_PCM);
    } else if (rtc_wire.ptt) {
      atomic_store(&rtc_wire.ptt_overrun, true);
    }
    mbedtls_platform_zeroize(frame, sizeof(*frame));
    rtc_wire.head = (rtc_wire.head + 1) % LAN_RTC_CAPTURE_SLOTS;
    --rtc_wire.count;
  }
  xSemaphoreGive(rtc_wire.capture_lock);
  return flags;
}

static size_t rtc_wire_seal(mbedtls_gcm_context *gcm, uint8_t *wire, uint32_t *sequence,
                            unsigned kind, unsigned flags, uint8_t *body, size_t bytes) {
  if (*sequence == UINT32_MAX) return 0;
  uint8_t nonce[12];
  pet_lan_rtc_header(wire, *sequence, kind, flags, bytes);
  pet_lan_rtc_nonce(nonce, PET_LAN_RTC_UP, (*sequence)++);
  const int result = mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, bytes, nonce, sizeof(nonce),
      wire, PET_LAN_RTC_HEADER, body, wire + PET_LAN_RTC_HEADER,
      LAN_TAG_SIZE, wire + PET_LAN_RTC_HEADER + bytes);
  return result ? 0 : PET_LAN_RTC_HEADER + bytes + LAN_TAG_SIZE;
}

static bool rtc_wire_send(int fd, mbedtls_gcm_context *gcm, uint8_t *wire, uint32_t *sequence,
                          unsigned kind, unsigned flags, uint8_t *body, size_t bytes) {
  const size_t size = rtc_wire_seal(gcm, wire, sequence, kind, flags, body, bytes);
  return size && socket_io(fd, wire, size, true, esp_timer_get_time() + LAN_RTC_WRITE_US);
}

static size_t rtc_wire_downlink_bytes(unsigned kind) {
  switch (kind) {
    case PET_LAN_RTC_KEEPALIVE: case PET_LAN_RTC_CLOSE: return 0;
    case LAN_RTC_BEGIN: case LAN_RTC_END: case LAN_RTC_FLUSH: return LAN_RTC_TURN_BYTES;
    case PET_LAN_RTC_AUDIO: return LAN_RTC_BODY_MAX;
    case PET_LAN_HUD_KIND: return PET_LAN_HUD_BYTES;
    default: return SIZE_MAX;
  }
}

static size_t rtc_wire_check_header(const uint8_t *wire, uint32_t sequence) {
  const unsigned kind = wire[0];
  const size_t bytes = rtc_wire_downlink_bytes(kind);
  uint8_t checked[PET_LAN_RTC_HEADER];
  if (bytes == SIZE_MAX || sequence == UINT32_MAX) return 0;
  pet_lan_rtc_header(checked, sequence, kind, 0, bytes);
  return memcmp(wire, checked, sizeof(checked)) ? 0 : PET_LAN_RTC_HEADER + bytes + LAN_TAG_SIZE;
}

static unsigned rtc_wire_open(mbedtls_gcm_context *gcm, uint8_t *wire,
                              uint8_t *body, uint32_t *sequence, size_t size) {
  const size_t bytes = size - PET_LAN_RTC_HEADER - LAN_TAG_SIZE;
  uint8_t nonce[12];
  pet_lan_rtc_nonce(nonce, PET_LAN_RTC_DOWN, (*sequence)++);
  return mbedtls_gcm_auth_decrypt(gcm, bytes, nonce, sizeof(nonce), wire, PET_LAN_RTC_HEADER,
      wire + PET_LAN_RTC_HEADER + bytes, LAN_TAG_SIZE, wire + PET_LAN_RTC_HEADER, body) ? 0 : wire[0];
}

static unsigned rtc_wire_receive_start(int fd, mbedtls_gcm_context *gcm, uint8_t *wire,
                                       uint8_t *body, uint32_t *sequence, int64_t deadline) {
  if (!socket_io(fd, wire, PET_LAN_RTC_HEADER, false, deadline)) return 0;
  const size_t size = rtc_wire_check_header(wire, *sequence);
  if (!size || !socket_io(fd, wire + PET_LAN_RTC_HEADER, size - PET_LAN_RTC_HEADER, false, deadline))
    return 0;
  return rtc_wire_open(gcm, wire, body, sequence, size);
}

static bool rtc_wire_retry_io(void) {
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

/* The audio task owns the descriptor after handoff. The management listener
 * never waits for media, audio ownership, or this task's cleanup. */
/* Capture-only socket. A normal authenticated CLOSE is emitted only after
 * physical release and after the microphone worker can no longer enqueue PCM. */
static void rtc_wire_ptt(int fd, mbedtls_gcm_context *gcm, uint8_t *wire, uint8_t *body,
                         uint32_t *rx, uint32_t *tx, uint32_t pair_generation) {
  const uint32_t token = rtc_wire.ptt;
  if (rtc_wire_receive_start(fd, gcm, wire, body, rx, esp_timer_get_time() + LAN_IO_US) !=
      PET_LAN_RTC_KEEPALIVE || !rtc_wire_trusted(pair_generation)) return;
  bool queue_empty = false;
  if (!lan_voice_claim(token, pair_generation, &queue_empty)) return;
  atomic_store(&voice_mic_token, token);
  const int power_save = pet_device_wifi_suspend_power_save();
  pet_p4_audio_wifi_session_t session = 0;
  bool ok = pet_p4_lan_voice_mic_allowed() &&
    pet_p4_audio_wifi_ptt_start(rtc_wire_tap, queue_empty, &session) == ESP_OK;
  if (ok) {
    body[0] = queue_empty;
    ok = rtc_wire_send(fd, gcm, wire, tx, LAN_RTC_PTT_READY, 0, body, 1);
  }
  while (ok && pet_p4_audio_wifi_live(session) &&
         pet_p4_lan_voice_mic_allowed() && rtc_wire_trusted(pair_generation)) {
    if (atomic_load(&rtc_wire.ptt_overrun)) { ok = false; break; }
    struct timeval timeout = {0, 0};
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    if (select(fd + 1, &readable, NULL, NULL, &timeout) != 0) { ok = false; break; }
    if (rtc_wire_take(body, false)) {
      ok = rtc_wire_send(fd, gcm, wire, tx, PET_LAN_RTC_AUDIO, 0, body, PET_LAN_RTC_PCM);
    } else {
      vTaskDelay(1);
    }
  }
  atomic_store(&voice_mic_token, 0);
  if (session) (void)pet_p4_audio_wifi_stop(session);
  if (ok && lan_voice_released(token) && rtc_wire_trusted(pair_generation)) {
    while (ok && rtc_wire_trusted(pair_generation) &&
           !atomic_load(&rtc_wire.ptt_overrun) && rtc_wire_take(body, false))
      ok = rtc_wire_send(fd, gcm, wire, tx, PET_LAN_RTC_AUDIO, 0, body, PET_LAN_RTC_PCM);
    if (ok && !atomic_load(&rtc_wire.ptt_overrun) && rtc_wire_trusted(pair_generation))
      rtc_wire_send(fd, gcm, wire, tx, PET_LAN_RTC_CLOSE, 0, body, 0);
  }
  pet_device_wifi_restore_power_save(power_save);
}

static void rtc_wire_task(void *unused) {
  (void)unused;
  const int fd = rtc_wire.fd;
  const uint32_t pair_generation = rtc_wire.generation;
  uint8_t wire[LAN_RTC_WIRE_MAX], outgoing[LAN_RTC_CAPTURE_BATCH * LAN_RTC_WIRE_MAX];
  uint8_t body[LAN_RTC_BODY_MAX];
  uint32_t rx = 0, tx = 0, turn = 0;
  size_t read_at = 0, read_goal = PET_LAN_RTC_HEADER, written = 0, write_goal = 0;
  int64_t read_until = 0, write_until = 0;
  bool play_open = false, close_requested = false;
  int previous_power_save = -1;
  uint32_t reason = 1;
  pet_p4_audio_wifi_session_t session = 0;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const uint8_t header[3] = {LAN_RTC_MODE, 0, rtc_wire.ptt ? LAN_VOICE_OFFER_BYTES : LAN_RTC_BOOT_BYTES};
  const bool admitted = rtc_wire_trusted(pair_generation);
  body[0] = admitted;
  bool ok = !mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, rtc_wire.handshake_key, 128) &&
    send_sealed_response(fd, &gcm, header, wire, body, 1, esp_timer_get_time() + LAN_IO_US);
  mbedtls_platform_zeroize(rtc_wire.handshake_key, sizeof(rtc_wire.handshake_key));
  ok = ok && admitted && rtc_wire_trusted(pair_generation) &&
    !mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, rtc_wire.media_key, 128);
  mbedtls_platform_zeroize(rtc_wire.media_key, sizeof(rtc_wire.media_key));
  if (!ok) goto cleanup;
  if (rtc_wire.ptt) {
    rtc_wire_ptt(fd, &gcm, wire, body, &rx, &tx, pair_generation);
    goto cleanup;
  }

  int64_t last = esp_timer_get_time();
  reason = 2;
  portENTER_CRITICAL(&rtc_hud_mux);
  rtc_hud_generation = pair_generation;
  pet_lan_hud_begin(&rtc_hud);
  portEXIT_CRITICAL(&rtc_hud_mux);
  /* Preparation is visible before TTS validation, without starting the mic.
   * Old peers still activate with their first Keepalive exactly as before. */
  for (;;) {
    if (esp_timer_get_time() - last >= PET_LAN_RTC_MAX_MS * INT64_C(1000)) goto cleanup;
    unsigned kind = rtc_wire_receive_start(fd, &gcm, wire, body, &rx,
        esp_timer_get_time() + PET_LAN_RTC_IDLE_MS * 1000);
    if (!rtc_wire_trusted(pair_generation)) goto cleanup;
    if (kind == PET_LAN_RTC_KEEPALIVE) break;
    if (kind == PET_LAN_RTC_CLOSE) { close_requested = true; reason = 0; goto cleanup; }
    if (kind != PET_LAN_HUD_KIND || !rtc_hud_offer(body) ||
        !rtc_wire_send(fd, &gcm, wire, &tx, PET_LAN_RTC_KEEPALIVE, 0, body, 0)) goto cleanup;
  }
  previous_power_save = pet_device_wifi_suspend_power_save();
  atomic_store(&rtc_wire_last_suspended_power_save, previous_power_save >= 0);
  reason = 3;
  if (pet_p4_audio_wifi_start(rtc_wire_tap, NULL, &session) != ESP_OK) {
    rtc_wire_send(fd, &gcm, wire, &tx, PET_LAN_RTC_CLOSE, 0, body, 0);
    goto cleanup;
  }
  if (!rtc_wire_trusted(pair_generation) ||
      !rtc_wire_send(fd, &gcm, wire, &tx, PET_LAN_RTC_KEEPALIVE, 0, body, 0)) goto cleanup;

  last = esp_timer_get_time();
  const int64_t end = last + PET_LAN_RTC_MAX_MS * INT64_C(1000);
  unsigned outgoing_reason = 0;
  reason = 4;  // ownership expired or codec stopped
  while (pet_p4_audio_wifi_live(session) && rtc_wire_trusted(pair_generation)) {
    const int64_t now = esp_timer_get_time();
    if (now >= end || now - last >= PET_LAN_RTC_IDLE_MS * 1000) { reason = 5; break; }
    if (read_at && now >= read_until) { reason = 12; break; }
    if (write_goal && now >= write_until) { reason = outgoing_reason; break; }

    // An incomplete write must not prevent reads and vice versa. In duplex
    // the peer can be waiting for our reads while the local send window is full.
    if (!write_goal) {
      const uint32_t drained = pet_p4_audio_wifi_take_drained_turn(session);
      if (drained) {
        ota_wire_put_u32(body, drained);
        outgoing_reason = 9;
        write_goal = rtc_wire_seal(&gcm, outgoing, &tx, LAN_RTC_END, 0, body, LAN_RTC_TURN_BYTES);
        if (!write_goal) { reason = 9; break; }
      } else {
        outgoing_reason = 10;
        for (unsigned i = 0; i < LAN_RTC_CAPTURE_BATCH; ++i) {
          const unsigned flags = rtc_wire_take(body, i == 0);
          if (!flags) break;
          // Preserve individual AEAD records and combine the TCP writes.
          const size_t sealed = rtc_wire_seal(&gcm, outgoing + write_goal, &tx,
              PET_LAN_RTC_AUDIO, flags, body, PET_LAN_RTC_PCM);
          if (!sealed) { reason = 10; goto cleanup; }
          write_goal += sealed;
        }
      }
      if (write_goal) {
        mbedtls_platform_zeroize(body, sizeof(body));
        write_until = now + LAN_RTC_WRITE_US;
      }
    }
    struct timeval timeout = {0, 10000};
    fd_set read_ready, write_ready;
    FD_ZERO(&read_ready);
    FD_ZERO(&write_ready);
    FD_SET(fd, &read_ready);
    if (write_goal) FD_SET(fd, &write_ready);
    const int ready = select(fd + 1, &read_ready, write_goal ? &write_ready : NULL, NULL, &timeout);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) { reason = 6; break; }
    if (ready && write_goal && FD_ISSET(fd, &write_ready)) {
      const int count = send(fd, outgoing + written, write_goal - written, 0);
      if (count > 0) {
        written += count;
        if (written == write_goal) written = write_goal = 0;
      } else if (!count || !rtc_wire_retry_io()) {
        reason = count ? 0x20000u | (uint16_t)errno : outgoing_reason;
        break;
      }
    }
    if (ready && FD_ISSET(fd, &read_ready)) {
      const int count = recv(fd, wire + read_at, read_goal - read_at, 0);
      if (count <= 0) {
        if (count < 0 && rtc_wire_retry_io()) continue;
        reason = 7;
        break;
      }
      if (!read_at) read_until = now + LAN_RTC_FRAME_US;
      read_at += count;
      if (read_at != read_goal) continue;
      if (read_goal == PET_LAN_RTC_HEADER) {
        read_goal = rtc_wire_check_header(wire, rx);
        if (!read_goal) { reason = 13; break; }
        continue;
      }
      const unsigned kind = rtc_wire_open(&gcm, wire, body, &rx, read_goal);
      read_at = 0;
      read_goal = PET_LAN_RTC_HEADER;
      if (!kind) { reason = 14; break; }
      if (!rtc_wire_trusted(pair_generation)) { reason = 11; break; }
      if (kind == PET_LAN_RTC_CLOSE) {
        close_requested = true;
        reason = 0;
        break;
      }
      last = esp_timer_get_time();
      if (kind == PET_LAN_HUD_KIND) {
        if (!rtc_hud_offer(body)) { reason = 15; break; }
      } else if (kind != PET_LAN_RTC_KEEPALIVE) {
        const uint32_t incoming = ota_wire_u32(body);
        if (!incoming || (incoming > turn && kind != LAN_RTC_BEGIN)) { reason = 8; break; }
        esp_err_t result = ESP_OK;
        if (kind == LAN_RTC_BEGIN && incoming > turn) {
          result = pet_p4_audio_wifi_play(session, incoming, PET_P4_AUDIO_WIFI_PLAY_BEGIN, NULL, 0);
          turn = incoming;
          play_open = true;
        } else if (incoming == turn && kind == PET_LAN_RTC_AUDIO && play_open) {
          /* Give the existing DAC worker one frame interval to free space. */
          result = pet_p4_audio_wifi_play(session, turn, PET_P4_AUDIO_WIFI_PLAY_FRAME,
                                         body + LAN_RTC_TURN_BYTES, PET_LAN_RTC_PCM);
          if (result == ESP_ERR_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(25));
            if (!rtc_wire_trusted(pair_generation)) break;
            result = pet_p4_audio_wifi_play(session, turn, PET_P4_AUDIO_WIFI_PLAY_FRAME,
                                           body + LAN_RTC_TURN_BYTES, PET_LAN_RTC_PCM);
          }
        } else if (incoming == turn && (kind == LAN_RTC_FLUSH || (kind == LAN_RTC_END && play_open))) {
          result = pet_p4_audio_wifi_play(session, turn,
            kind == LAN_RTC_FLUSH ? PET_P4_AUDIO_WIFI_PLAY_FLUSH : PET_P4_AUDIO_WIFI_PLAY_END, NULL, 0);
          play_open = false;
        }
        if (result != ESP_OK) { reason = 0x10000u | (uint16_t)result; break; }
      }
      mbedtls_platform_zeroize(body, sizeof(body));
    }
  }

cleanup:
  if (!close_requested) close(fd);
  if ((!session || pet_p4_audio_wifi_stop(session) == ESP_OK) && close_requested &&
      rtc_wire_trusted(pair_generation) &&
      // Complete the sealed record first. Its sequence may already be visible.
      (!write_goal || socket_io(fd, outgoing + written, write_goal - written, true, write_until))) {
    rtc_wire_send(fd, &gcm, wire, &tx, PET_LAN_RTC_CLOSE, 0, body, 0);
  }
  if (close_requested) close(fd);
  if (!rtc_wire.ptt) {
    const bool hud_closed = close_requested && rtc_wire_trusted(pair_generation);
    portENTER_CRITICAL(&rtc_hud_mux);
    if (hud_closed) pet_lan_hud_close(&rtc_hud);
    else pet_lan_hud_end(&rtc_hud);
    portEXIT_CRITICAL(&rtc_hud_mux);
  }
  mbedtls_gcm_free(&gcm);
  mbedtls_platform_zeroize(wire, sizeof(wire));
  mbedtls_platform_zeroize(outgoing, sizeof(outgoing));
  mbedtls_platform_zeroize(body, sizeof(body));
  mbedtls_platform_zeroize(rtc_wire.queue, sizeof(rtc_wire.queue));
  rtc_wire.head = rtc_wire.count = 0;
  vSemaphoreDelete(rtc_wire.capture_lock);
  pet_device_wifi_restore_power_save(previous_power_save);
  if (!rtc_wire.ptt) atomic_store(&rtc_wire_last_exit, reason);
  atomic_store(&rtc_wire_busy, false);
  vTaskDelete(NULL);
}

/* Return true only when the worker owns fd and the caller must skip close. */
static bool rtc_wire_handoff(int fd, const uint8_t *offer, size_t size, const uint8_t *master,
                             const uint8_t *transcript, uint32_t pair_generation) {
  const char *current_boot = pet_p4_boot_identity();
  if (fd < 0 || fd >= LWIP_SELECT_MAXNFDS || strlen(current_boot) != LAN_RTC_BOOT_BYTES ||
      memcmp(offer, current_boot, LAN_RTC_BOOT_BYTES) || !rtc_wire_trusted(pair_generation)) return false;
  const uint32_t ptt = size == LAN_VOICE_OFFER_BYTES ? ota_wire_u32(offer + LAN_RTC_BOOT_BYTES) : 0;
  if (size == LAN_VOICE_OFFER_BYTES) {
    uint8_t available[LAN_VOICE_STATUS_BYTES] = {0};
    lan_voice_status(pair_generation, available);
    if (!ptt || ptt != ota_wire_u32(available)) return false;
  }
  if (atomic_exchange(&rtc_wire_busy, true)) return false;
  static const uint8_t context[] = "P4LAN-RTC-v1";
  const size_t prefix = sizeof(context) - 1 + 2 * LAN_NONCE_SIZE;
  uint8_t input[sizeof(context) - 1 + 2 * LAN_NONCE_SIZE + LAN_VOICE_OFFER_BYTES], digest[32];
  memcpy(input, context, sizeof(context) - 1);
  memcpy(input + sizeof(context) - 1, transcript + sizeof(domain) - 1, 2 * LAN_NONCE_SIZE);
  memcpy(input + prefix, offer, size);
  const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool ok = !mbedtls_md_hmac(sha256, master, LAN_KEY_SIZE, input, prefix + size, digest);
  if (ok) {
    memcpy(rtc_wire.media_key, digest, LAN_KEY_SIZE);
    ok = !mbedtls_md_hmac(sha256, master, LAN_KEY_SIZE, transcript,
                         sizeof(domain) - 1 + 2 * LAN_NONCE_SIZE, digest);
  }
  if (ok) memcpy(rtc_wire.handshake_key, digest, LAN_KEY_SIZE);
  mbedtls_platform_zeroize(digest, sizeof(digest));
  const int enable = 1;
  if (ok && setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) == 0) {
    rtc_wire.fd = fd;
    rtc_wire.generation = pair_generation;
    rtc_wire.ptt = ptt;
    atomic_store(&rtc_wire.ptt_overrun, false);
    rtc_wire.capture_lock = xSemaphoreCreateMutex();
    if (rtc_wire.capture_lock &&
        xTaskCreate(rtc_wire_task, "pet_lan_rtc", 6144, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS)
      return true;
    if (rtc_wire.capture_lock) vSemaphoreDelete(rtc_wire.capture_lock);
  }
  mbedtls_platform_zeroize(rtc_wire.handshake_key, sizeof(rtc_wire.handshake_key));
  mbedtls_platform_zeroize(rtc_wire.media_key, sizeof(rtc_wire.media_key));
  atomic_store(&rtc_wire_busy, false);
  return false;
}
