#include "pet_p4_lan.h"
#include "pet_p4_private_feed.h"

#if PET_P4_SYSTEM_WIFI
// Network control only: keep the renderer and interpreter optimization unchanged.
#pragma GCC optimize ("Oz")
#include "pet_p4_device_wifi.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_lan_context.h"
#include "pet_p4_lan_press.h"
#include "pet_p4_json_source.h"
#include "pet_p4_ota.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_widget_data.h"
#include "pet_p4_wifi_clock.h"
#include "esp_app_desc.h"
#include "esp_srp.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/bignum.h"
#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "nvs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

#define LAN_PORT 29461
#define LAN_MAX_BODY 16384
#define LAN_STATUS_MODE 6
#define LAN_PRESS_MODE 16
#define LAN_PRIVATE_FEED_MODE 21
#define LAN_VOLUME_REQUEST 11
#define LAN_VOLUME_QUERY 8
#define LAN_STATUS_MAX 576
#define LAN_KEYBOARD_REQUEST_MAX 64
#define LAN_KEYBOARD_REPLY_MAX 512
#define LAN_PANEL_REQUEST_MAX 8192
#define LAN_PANEL_REPLY_MAX (LAN_MAX_BODY - 2)
#define LAN_APPEARANCE_REPLY_MAX 3072
#define LAN_WIDGET_DATA_MAX 10240
#define LAN_WIDGET_BOARD_BYTES 15
#define LAN_WIDGET_PREFIX_BYTES (16 + LAN_WIDGET_BOARD_BYTES)
#define LAN_LEASE_MS 10000
#define LAN_IO_US 1800000
#define LAN_KEY_SIZE 16
#define LAN_TAG_SIZE 16
#define LAN_NONCE_SIZE 16
#define LAN_SRP_PUBLIC_BYTES 384
#define LAN_SRP_PROOF_BYTES 64
#define LAN_SRP_SALT_BYTES 16
#define LAN_SRP_DEADLINE_US 8000000

static const char domain[] = "P4LAN1";
static SemaphoreHandle_t lock;
static TaskHandle_t worker;
static uint8_t paired_key[LAN_KEY_SIZE];
static bool paired, failed, pairing, state_known;
static atomic_bool forget_pending;
static uint32_t generation;
static char address[16];
typedef struct {
  cJSON *root;
  uint64_t received;
  uint8_t status_hash[32];
  uint8_t speech_hash[32];
} lan_update_t;
static lan_update_t mailbox;
static uint64_t client_seen_ms;
static atomic_bool usb_client_active;
static pet_lan_press_t realtime_press;
void pet_p4_lan_usb_active(void) { atomic_store(&usb_client_active, true); }
static _Atomic(cJSON *) retired;
static pet_lan_volume_t volume_control;

static uint64_t clock_ms(void) { return esp_timer_get_time() / 1000; }

static bool volume_available(void) {
  return paired && !atomic_load(&forget_pending) && pet_device_wifi_online();
}

#include "pet_p4_lan_trust.inc"
#include "pet_p4_lan_panel.inc"
#include "pet_p4_lan_component.inc"

static void refresh_volume(void) {
  const uint64_t now = clock_ms();
  if (!volume_available() || !atomic_load(&control_trusted))
    pet_lan_volume_cancel(&volume_control, now);
  pet_lan_volume_refresh(&volume_control, now);
}

bool pet_p4_lan_volume_claim(uint8_t *volume) {
  if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) return false;
  refresh_volume();
  const bool claimed = pet_lan_volume_claim(&volume_control, clock_ms(), volume);
  xSemaphoreGive(lock);
  return claimed;
}

void pet_p4_lan_volume_finish(bool applied) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (volume_control.phase == PET_LAN_VOLUME_RUNNING) {
    volume_control.phase = applied ? PET_LAN_VOLUME_APPLIED : PET_LAN_VOLUME_FAILED;
    volume_control.expires = clock_ms() + PET_LAN_VOLUME_WINDOW_MS;
  }
  xSemaphoreGive(lock);
}

static bool store_key(const uint8_t *key) {
  nvs_handle_t handle;
  if (nvs_open("p4_agent_lan", NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t result = key ? nvs_set_blob(handle, "key", key, LAN_KEY_SIZE)
                         : nvs_erase_key(handle, "key");
  if (!key && result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
  if (result == ESP_OK) {
    result = nvs_erase_key(handle, "wired_trust");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK;
}

void pet_p4_lan_forget(void) {
  atomic_store(&forget_pending, true);
}

void pet_p4_lan_pair_view(pet_lan_pair_view_t *view) {
  memset(view, 0, sizeof(*view));
  if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) {
    view->busy = true;
    return;
  }
  memcpy(view->ip, address, sizeof(address));
  refresh_volume();
  view->busy = pairing || atomic_load(&forget_pending);
  view->paired = paired && !view->busy;
  view->failed = failed;
  view->ready = pet_p4_lan_can_pair(address[0] != 0, state_known, paired, view->busy);
  xSemaphoreGive(lock);
}

static bool socket_io(int fd, void *buffer, size_t size, bool write, int64_t deadline) {
  uint8_t *p = buffer;
  while (size) {
    const int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0) return false;
    struct timeval timeout = {remaining / 1000000, remaining % 1000000};
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(fd, &ready);
    const int selected = select(fd + 1, write ? NULL : &ready, write ? &ready : NULL, NULL, &timeout);
    if (selected < 0 && errno == EINTR) continue;
    if (selected <= 0) return false;
    const int n = write ? send(fd, p, size, 0) : recv(fd, p, size, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (n <= 0) return false;
    p += n;
    size -= n;
  }
  return true;
}

static bool valid_source(const cJSON *value) {
  if (!cJSON_IsString(value) || !value->valuestring[0]) return false;
  const char *s = value->valuestring;
  if (strlen(s) >= PET_P4_AGENT_MAX) return false;
  for (; *s; ++s)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-'))
      return false;
  return true;
}

static bool valid_snapshot(const cJSON *root) {
  if (!cJSON_IsObject(root)) return false;
  const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
  const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
  const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(root, "sessions");
  const cJSON *speech = cJSON_GetObjectItemCaseSensitive(root, "speech");
  const cJSON *pixels = cJSON_GetObjectItemCaseSensitive(root, "textPixels");
  const bool masks_only = cJSON_IsObject(pixels) &&
    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(pixels, "bodyMasks"));
  if (!valid_source(source) || (!status && !sessions && !speech && !masks_only) ||
      (status && !cJSON_IsObject(status)) || (speech && !cJSON_IsObject(speech)))
    return false;
  if (sessions) {
    const cJSON *agent = cJSON_GetObjectItemCaseSensitive(sessions, "agentId");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(sessions, "sessions");
    if (!cJSON_IsObject(sessions) || !cJSON_IsString(agent) ||
        strcmp(agent->valuestring, source->valuestring) || !cJSON_IsArray(rows) ||
        cJSON_GetArraySize(rows) > PET_P4_SESSION_QUEUE_MAX) return false;
  }
  /* Reject unknown or duplicate envelope keys. */
  return (!pixels || cJSON_IsObject(pixels) || cJSON_IsFalse(pixels)) &&
    cJSON_GetArraySize(root) == 1 + (status != NULL) + (sessions != NULL) + (speech != NULL) + (pixels != NULL);
}

static bool payload_hash(const cJSON *root, const char *field, uint8_t hash[32]) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, field);
  if (!value) return true;
  char *text = cJSON_PrintUnformatted(value);
  if (!text) return false;
  bool ok = mbedtls_sha256((const unsigned char *)text, strlen(text), hash, 0) == 0;
  cJSON_free(text);
  return ok;
}

/* The ESP-IDF helper strips leading zeroes from its random salt but keeps
 * the requested length in its proof context. Set the fixed-length salt and
 * verifier explicitly. This also keeps the PIN out of SDK debug logging. */
static bool set_pair_verifier(esp_srp_handle_t *srp, const char *pin, uint8_t salt[LAN_SRP_SALT_BYTES]) {
  /* RFC 5054 Appendix A, the same group used by ESP_NG_3072. */
  static const uint8_t prime[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2, 0x21, 0x68, 0xc2, 0x34,
    0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1, 0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74,
    0x02, 0x0b, 0xbe, 0xa6, 0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d, 0xf2, 0x5f, 0x14, 0x37,
    0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45, 0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6,
    0xf4, 0x4c, 0x42, 0xe9, 0xa6, 0x37, 0xed, 0x6b, 0x0b, 0xff, 0x5c, 0xb6, 0xf4, 0x06, 0xb7, 0xed,
    0xee, 0x38, 0x6b, 0xfb, 0x5a, 0x89, 0x9f, 0xa5, 0xae, 0x9f, 0x24, 0x11, 0x7c, 0x4b, 0x1f, 0xe6,
    0x49, 0x28, 0x66, 0x51, 0xec, 0xe4, 0x5b, 0x3d, 0xc2, 0x00, 0x7c, 0xb8, 0xa1, 0x63, 0xbf, 0x05,
    0x98, 0xda, 0x48, 0x36, 0x1c, 0x55, 0xd3, 0x9a, 0x69, 0x16, 0x3f, 0xa8, 0xfd, 0x24, 0xcf, 0x5f,
    0x83, 0x65, 0x5d, 0x23, 0xdc, 0xa3, 0xad, 0x96, 0x1c, 0x62, 0xf3, 0x56, 0x20, 0x85, 0x52, 0xbb,
    0x9e, 0xd5, 0x29, 0x07, 0x70, 0x96, 0x96, 0x6d, 0x67, 0x0c, 0x35, 0x4e, 0x4a, 0xbc, 0x98, 0x04,
    0xf1, 0x74, 0x6c, 0x08, 0xca, 0x18, 0x21, 0x7c, 0x32, 0x90, 0x5e, 0x46, 0x2e, 0x36, 0xce, 0x3b,
    0xe3, 0x9e, 0x77, 0x2c, 0x18, 0x0e, 0x86, 0x03, 0x9b, 0x27, 0x83, 0xa2, 0xec, 0x07, 0xa2, 0x8f,
    0xb5, 0xc5, 0x5d, 0xf0, 0x6f, 0x4c, 0x52, 0xc9, 0xde, 0x2b, 0xcb, 0xf6, 0x95, 0x58, 0x17, 0x18,
    0x39, 0x95, 0x49, 0x7c, 0xea, 0x95, 0x6a, 0xe5, 0x15, 0xd2, 0x26, 0x18, 0x98, 0xfa, 0x05, 0x10,
    0x15, 0x72, 0x8e, 0x5a, 0x8a, 0xaa, 0xc4, 0x2d, 0xad, 0x33, 0x17, 0x0d, 0x04, 0x50, 0x7a, 0x33,
    0xa8, 0x55, 0x21, 0xab, 0xdf, 0x1c, 0xba, 0x64, 0xec, 0xfb, 0x85, 0x04, 0x58, 0xdb, 0xef, 0x0a,
    0x8a, 0xea, 0x71, 0x57, 0x5d, 0x06, 0x0c, 0x7d, 0xb3, 0x97, 0x0f, 0x85, 0xa6, 0xe1, 0xe4, 0xc7,
    0xab, 0xf5, 0xae, 0x8c, 0xdb, 0x09, 0x33, 0xd7, 0x1e, 0x8c, 0x94, 0xe0, 0x4a, 0x25, 0x61, 0x9d,
    0xce, 0xe3, 0xd2, 0x26, 0x1a, 0xd2, 0xee, 0x6b, 0xf1, 0x2f, 0xfa, 0x06, 0xd9, 0x8a, 0x08, 0x64,
    0xd8, 0x76, 0x02, 0x73, 0x3e, 0xc8, 0x6a, 0x64, 0x52, 0x1f, 0x2b, 0x18, 0x17, 0x7b, 0x20, 0x0c,
    0xbb, 0xe1, 0x17, 0x57, 0x7a, 0x61, 0x5d, 0x6c, 0x77, 0x09, 0x88, 0xc0, 0xba, 0xd9, 0x46, 0xe2,
    0x08, 0xe2, 0x4f, 0xa0, 0x74, 0xe5, 0xab, 0x31, 0x43, 0xdb, 0x5b, 0xfc, 0xe0, 0xfd, 0x10, 0x8e,
    0x4b, 0x82, 0xd1, 0x20, 0xa9, 0x3a, 0xd2, 0xca, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  };
  char identity_password[64];
  const size_t identity_len = pet_p4_lan_identity_password(identity_password, sizeof(identity_password), pin);
  if (!identity_len) return false;
  uint8_t input[LAN_SRP_SALT_BYTES + LAN_SRP_PROOF_BYTES], hash[LAN_SRP_PROOF_BYTES];
  uint8_t verifier[LAN_SRP_PUBLIC_BYTES];
  esp_fill_random(salt, LAN_SRP_SALT_BYTES);
  memcpy(input, salt, LAN_SRP_SALT_BYTES);

  mbedtls_mpi n, g, x, v;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&g);
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&v);
  const bool ok = mbedtls_sha512((const uint8_t *)identity_password, identity_len,
      input + LAN_SRP_SALT_BYTES, 0) == 0 &&
    mbedtls_sha512(input, sizeof(input), hash, 0) == 0 &&
    mbedtls_mpi_read_binary(&n, prime, sizeof(prime)) == 0 &&
    mbedtls_mpi_lset(&g, 5) == 0 &&
    mbedtls_mpi_read_binary(&x, hash, sizeof(hash)) == 0 &&
    mbedtls_mpi_exp_mod(&v, &g, &x, &n, NULL) == 0 &&
    mbedtls_mpi_write_binary(&v, verifier, sizeof(verifier)) == 0 &&
    esp_srp_set_salt_verifier(srp, (const char *)salt, LAN_SRP_SALT_BYTES,
      (const char *)verifier, sizeof(verifier)) == ESP_OK;
  mbedtls_mpi_free(&v);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&g);
  mbedtls_mpi_free(&n);
  mbedtls_platform_zeroize(verifier, sizeof(verifier));
  mbedtls_platform_zeroize(identity_password, sizeof(identity_password));
  mbedtls_platform_zeroize(input, sizeof(input));
  mbedtls_platform_zeroize(hash, sizeof(hash));
  return ok;
}

/* Only an unconfigured device accepts first enrollment. Authentication and a
 * durable write complete before the client receives its success proof. */
static void pair_unconfigured(int fd, uint8_t *public_a, const uint8_t *transcript, size_t transcript_len) {
  const int64_t deadline = esp_timer_get_time() + LAN_SRP_DEADLINE_US;
  if (!socket_io(fd, public_a, LAN_SRP_PUBLIC_BYTES, false, deadline)) return;
  /* The client samples A below 2^3071, strictly below the 3072-bit SRP prime. */
  unsigned prefix = 0;
  for (unsigned i = 0; i + 1 < LAN_SRP_PUBLIC_BYTES; i++) prefix |= public_a[i];
  if ((public_a[0] & 0x80) || (!prefix && public_a[LAN_SRP_PUBLIC_BYTES - 1] < 2)) return;

  const char *pin = PET_P4_LAN_AUTO_CONTEXT;
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool allowed = pet_p4_lan_can_pair(pet_device_wifi_online(), state_known, paired,
    pairing || atomic_load(&forget_pending));
  const uint32_t request_generation = generation;
  if (allowed) { pairing = true; failed = false; }
  xSemaphoreGive(lock);
  if (!allowed) return;

  char identity[] = "Hachimodock";
  esp_srp_handle_t *srp = esp_srp_init(ESP_NG_3072);
  char *public_b = NULL, *secret = NULL;
  uint8_t salt[LAN_SRP_SALT_BYTES];
  int public_b_len = 0;
  uint16_t secret_len = 0;
  uint8_t proof[LAN_SRP_PROOF_BYTES], response[LAN_SRP_PROOF_BYTES], master[32];
  bool ok = srp && set_pair_verifier(srp, pin, salt) &&
    esp_srp_srv_pubkey_from_salt_verifier(srp, &public_b, &public_b_len) == ESP_OK;

  if (ok && public_b_len > 0 && public_b_len <= LAN_SRP_PUBLIC_BYTES) {
    uint8_t length[] = {(uint8_t)(public_b_len >> 8), (uint8_t)public_b_len};
    ok = socket_io(fd, salt, LAN_SRP_SALT_BYTES, true, deadline) &&
      socket_io(fd, length, sizeof(length), true, deadline) &&
      socket_io(fd, public_b, (size_t)public_b_len, true, deadline) &&
      esp_srp_get_session_key(srp, (char *)public_a, LAN_SRP_PUBLIC_BYTES, &secret, &secret_len) == ESP_OK &&
      secret_len == LAN_SRP_PROOF_BYTES && socket_io(fd, proof, sizeof(proof), false, deadline) &&
      esp_srp_exchange_proofs(srp, identity, sizeof(identity) - 1, (char *)proof, (char *)response) == ESP_OK &&
      mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)secret, secret_len,
        transcript, transcript_len, master) == 0;
  } else ok = false;

  if (ok) {
    xSemaphoreTake(lock, portMAX_DELAY);
    ok = request_generation == generation &&
      pet_p4_lan_can_pair(pet_device_wifi_online(), state_known, paired, atomic_load(&forget_pending)) &&
      esp_timer_get_time() < deadline && store_key(master);
    if (ok) {
      memcpy(paired_key, master, sizeof(paired_key));
      paired = true;
      atomic_store(&control_trusted, false);
      pet_lan_press_clear(&realtime_press);
      failed = false;
      ++generation;
      client_seen_ms = 0;
      cJSON_Delete(mailbox.root);
      mailbox.root = NULL;
    }
    xSemaphoreGive(lock);
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  pairing = false;
  failed = !ok;
  xSemaphoreGive(lock);
  if (ok) socket_io(fd, response, sizeof(response), true, deadline);
  if (secret) mbedtls_platform_zeroize(secret, secret_len);
  if (srp) esp_srp_free(srp);
  mbedtls_platform_zeroize(master, sizeof(master));
  mbedtls_platform_zeroize(proof, sizeof(proof));
  mbedtls_platform_zeroize(response, sizeof(response));
}

/* Status reads use board-owned snapshots. They do not bind protocol/audio
 * callbacks to a transient socket or take the runtime state mutex. */
static bool send_sealed_response(int fd, mbedtls_gcm_context *gcm, const uint8_t header[3],
                                  uint8_t *cipher, uint8_t *plain, size_t length, int64_t deadline) {
  const uint8_t nonce[12] = {1};
  uint8_t aad[5] = {header[0], header[1], header[2], length >> 8, length & 0xff};
  const bool sealed = mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, length,
    nonce, sizeof(nonce), aad, sizeof(aad), plain, cipher + 2, LAN_TAG_SIZE, cipher + 2 + length) == 0;
  mbedtls_platform_zeroize(plain, length);
  memcpy(cipher, aad + 3, 2);
  return sealed && socket_io(fd, cipher, 2 + length + LAN_TAG_SIZE, true, deadline);
}

#include "pet_p4_lan_ota.inc"
#include "pet_p4_lan_voice.inc"
#include "pet_p4_lan_realtime.inc"
#include "pet_p4_lan_component_wire.inc"

__attribute__((optimize("Oz")))
static bool send_device_status(int fd, mbedtls_gcm_context *gcm, const uint8_t header[3],
                               uint8_t *cipher, uint8_t *plain, int64_t deadline, bool extended) {
  const esp_app_desc_t *app = esp_app_get_description();
  const pet_wifi_link_status_t link = pet_device_wifi_link_status();
  uint64_t utc, age;
  int32_t offset;
  const bool clock_ready = pet_wifi_clock_read(clock_ms(), &utc, &offset, &age);
  char board[16] = {0};
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK)
    snprintf(board, sizeof(board), "p4-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cJSON *status = cJSON_CreateObject();
  if (!status) return false;
  const bool built = cJSON_AddStringToObject(status, "firmware", app ? app->version : "") &&
    cJSON_AddStringToObject(status, "boardDeviceId", board) &&
    cJSON_AddStringToObject(status, "bootId", pet_p4_boot_identity()) &&
    cJSON_AddBoolToObject(status, "wifiConnected", pet_device_wifi_online()) &&
    cJSON_AddBoolToObject(status, "clockSynchronized", clock_ready) &&
    cJSON_AddBoolToObject(status, "trustedVolume", true) &&
    cJSON_AddBoolToObject(status, "componentDelivery", true) &&
    cJSON_AddBoolToObject(status, "rebootSupported", true) &&
    cJSON_AddBoolToObject(status, "wirelessOta", true) &&
    cJSON_AddBoolToObject(status, "wirelessRealtime", true) &&
    cJSON_AddNumberToObject(status, "rtcKey", atomic_load(&usb_client_active) ? 7 : 5) &&
    cJSON_AddBoolToObject(status, "wirelessRealtimeBusy", atomic_load(&rtc_wire_busy)) &&
    cJSON_AddNumberToObject(status, "wirelessRealtimeLastExit", atomic_load(&rtc_wire_last_exit)) &&
    cJSON_AddBoolToObject(status, "wirelessRealtimePowerSaveSuspended",
                          atomic_load(&rtc_wire_last_suspended_power_save)) &&
    cJSON_AddBoolToObject(status, "componentBundle", true) &&
    cJSON_AddBoolToObject(status, "inputDoublePress", true) &&
    cJSON_AddBoolToObject(status, "ops", true) &&
    cJSON_AddBoolToObject(status, "controlTrusted", atomic_load(&control_trusted) && !atomic_load(&forget_pending)) &&
    cJSON_AddNumberToObject(status, "httpsSuccesses", link.successes) &&
    cJSON_AddNumberToObject(status, "httpsFailures", link.failures);
  const bool encoded = built &&
    (!extended || (cJSON_AddNumberToObject(status, "ac", (double)pet_p4_raw_assets_capacity_bytes()) &&
      cJSON_AddBoolToObject(status, "wd", true) &&
      cJSON_AddBoolToObject(status, "ptt", true) &&
      cJSON_AddBoolToObject(status, "pf", pet_private_storage_ready()))) &&
    cJSON_PrintPreallocated(status, (char *)plain, LAN_STATUS_MAX, false);
  cJSON_Delete(status);
  if (!encoded) return false;
  return send_sealed_response(fd, gcm, header, cipher, plain, strlen((char *)plain), deadline);
}

static void handle_peer(int fd, uint8_t *cipher, uint8_t *plain, bool *handed_off) {
  const int64_t deadline = esp_timer_get_time() + LAN_IO_US;
  uint8_t transcript[sizeof(domain) - 1 + 2 * LAN_NONCE_SIZE];
  memcpy(transcript, domain, sizeof(domain) - 1);
  uint8_t *client_nonce = transcript + sizeof(domain) - 1;
  uint8_t *server_nonce = client_nonce + LAN_NONCE_SIZE;
  uint8_t header[3];
  if (!socket_io(fd, client_nonce, LAN_NONCE_SIZE, false, deadline)) return;
  esp_fill_random(server_nonce, LAN_NONCE_SIZE);
  if (!socket_io(fd, server_nonce, LAN_NONCE_SIZE, true, deadline) ||
      !socket_io(fd, header, sizeof(header), false, deadline)) return;
  const unsigned mode = header[0];
  const size_t size = ((size_t)header[1] << 8) | header[2];
  if (mode == 5) {
    if (size == LAN_SRP_PUBLIC_BYTES) pair_unconfigured(fd, cipher, transcript, sizeof(transcript));
    return;
  }
  switch (mode) {
    case 0: case LAN_OTA_MODE: if (size) return; break;
    case LAN_STATUS_MODE: if (size > 1) return; break;
    case 2: if (!size || size > LAN_MAX_BODY) return; break;
    case LAN_PRESS_MODE: if (size != 36) return; break;
    case LAN_PRIVATE_FEED_MODE: if (size < 25 || size > PET_PRIVATE_FEED_REQUEST_MAX) return; break;
    case LAN_VOLUME_REQUEST:
      if (size != 16 + PET_LAN_VOLUME_ID_SIZE + 1) return;
      break;
    case LAN_VOLUME_QUERY: if (size != PET_LAN_VOLUME_ID_SIZE) return; break;
    case LAN_RTC_MODE:
      if (size != LAN_RTC_BOOT_BYTES && size != LAN_VOICE_OFFER_BYTES) return;
      break;
    case PET_LAN_CONTROL_REBOOT: case LAN_BUNDLE_MODE: case LAN_VOICE_MODE:
      if (size != 16) return;
      break;
    case PET_LAN_CONTROL_COMPONENT: if (size < 24 || size > LAN_COMPONENT_MAX) return; break;
    case PET_LAN_CONTROL_KEYBOARD: if (size < 18 || size > 16 + LAN_KEYBOARD_REQUEST_MAX) return; break;
    case PET_LAN_CONTROL_PANEL: if (size < 17 || size > LAN_PANEL_REQUEST_MAX) return; break;
    case PET_LAN_CONTROL_APPEARANCE: if (size < 19 || size > LAN_MAX_BODY - 1) return; break;
    case PET_LAN_CONTROL_WIDGET_DATA:
      if (size <= LAN_WIDGET_PREFIX_BYTES || size > LAN_WIDGET_DATA_MAX) return;
      break;
    default: return;
  }
  if (!socket_io(fd, cipher, size + LAN_TAG_SIZE, false, deadline)) return;

  uint8_t master[LAN_KEY_SIZE], digest[32], nonce[12] = {0};
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool allowed = paired;
  const uint32_t request_generation = generation;
  if (allowed) memcpy(master, paired_key, sizeof(master));
  xSemaphoreGive(lock);
  if (!allowed) return;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), master,
    sizeof(master), transcript, sizeof(transcript), digest) == 0 &&
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, digest, 128) == 0 &&
    mbedtls_gcm_auth_decrypt(&gcm, size, nonce, sizeof(nonce), header, sizeof(header),
      cipher + size, LAN_TAG_SIZE, cipher, plain) == 0;
  mbedtls_platform_zeroize(digest, sizeof(digest));

  lan_update_t update = {0};
  uint8_t volume_result = PET_LAN_VOLUME_NONE;
  if (ok && mode == 2) {
    plain[size] = 0;
    if (pet_json_source_text_limit_bytes(plain, size, 8))
      update.root = cJSON_ParseWithOpts((char *)plain, NULL, true);
    ok = valid_snapshot(update.root) &&
      payload_hash(update.root, "status", update.status_hash) &&
      payload_hash(update.root, "speech", update.speech_hash);
    mbedtls_platform_zeroize(plain, size);
  }
  if (ok) {
    xSemaphoreTake(lock, portMAX_DELAY);
    ok = request_generation == generation && !atomic_load(&forget_pending) && pet_device_wifi_online();
    const bool usb_active = atomic_load(&usb_client_active);
    if (ok && !usb_active && (mode == 0 || mode == 2)) client_seen_ms = clock_ms();
    if (ok && mode == 2) {
      cJSON_Delete(mailbox.root);
      update.received = clock_ms();
      mailbox = update;
      update.root = NULL;
    }
    if (ok && atomic_load(&control_trusted) &&
        (mode == LAN_VOLUME_QUERY || (mode == LAN_VOLUME_REQUEST && !usb_active))) {
      refresh_volume();
      const uint64_t now = clock_ms();
      if (mode == LAN_VOLUME_QUERY) {
        volume_result = pet_lan_volume_query(&volume_control, now, plain);
      } else if (strlen(pet_p4_boot_identity()) == 16 &&
                 !memcmp(plain, pet_p4_boot_identity(), 16) &&
                 pet_lan_volume_submit(&volume_control, now, true, plain + 16, plain[size - 1])) {
        volume_result = PET_LAN_VOLUME_APPROVED;
      }
    }
    xSemaphoreGive(lock);
  }
  if (mode == LAN_VOLUME_REQUEST) mbedtls_platform_zeroize(plain, size);
  const bool wired = atomic_load(&usb_client_active);
  if (ok && mode == LAN_PRESS_MODE) {
    uint8_t reply[12] = {0};
    const uint32_t ack = ((uint32_t)plain[32] << 24) | ((uint32_t)plain[33] << 16) |
      ((uint32_t)plain[34] << 8) | plain[35];
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint64_t now = clock_ms();
    const bool admitted = request_generation == generation && paired && !atomic_load(&forget_pending) &&
      atomic_load(&control_trusted) && pet_device_wifi_online() && !atomic_load(&usb_client_active);
    pet_lan_press_refresh(&realtime_press, now, admitted);
    if (admitted && strlen(pet_p4_boot_identity()) == 16 && !memcmp(plain, pet_p4_boot_identity(), 16) &&
        pet_lan_press_poll(&realtime_press, now, plain + 16, ack)) {
      client_seen_ms = now;
      reply[0] = 1;
      const pet_lan_press_event_t *event = realtime_press.count ? &realtime_press.events[0] : NULL;
      const uint32_t seq = event ? event->seq : 0;
      const uint16_t ttl = event ? (uint16_t)(event->until - now) : 0;
      reply[1] = event ? event->context : 0; reply[2] = ttl >> 8; reply[3] = ttl;
      for (unsigned i = 0; i < 4; ++i) {
        reply[4+i] = seq >> (24 - 8*i);
        reply[8+i] = realtime_press.acknowledged >> (24 - 8*i);
      }
    }
    xSemaphoreGive(lock);
    send_sealed_response(fd, &gcm, header, cipher, reply, sizeof(reply), deadline);
  } else if (ok && mode == LAN_PRIVATE_FEED_MODE) {
    uint8_t reply = PET_PRIVATE_REFUSED;
    if (strlen(pet_p4_boot_identity()) == 16 && !memcmp(plain, pet_p4_boot_identity(), 16) &&
        !pet_p4_ota_active() && ota_wire_trusted(request_generation)) {
      reply = pet_private_credentials_request(plain + 16, size - 16);
    }
    mbedtls_platform_zeroize(plain, size);
    send_sealed_response(fd, &gcm, header, cipher, &reply, sizeof(reply), deadline);
  } else if (ok && mode == LAN_VOICE_MODE) {
    uint8_t reply[LAN_VOICE_STATUS_BYTES] = {0};
    if (rtc_wire_trusted(request_generation) &&
        !memcmp(plain, pet_p4_boot_identity(), LAN_RTC_BOOT_BYTES)) {
      lan_voice_status(request_generation, reply);
      send_sealed_response(fd, &gcm, header, cipher, reply, sizeof(reply), deadline);
    }
  } else if (ok && mode == LAN_RTC_MODE) {
    *handed_off = !wired && !pet_p4_ota_active() && !pet_p4_diagnostics_reboot_pending() &&
      rtc_wire_handoff(fd, plain, size, master, transcript, request_generation);
    if (!*handed_off) {
      plain[0] = 0;
      send_sealed_response(fd, &gcm, header, cipher, plain, 1, deadline);
    }
  } else if (ok && mode == LAN_BUNDLE_MODE) {
    const bool admitted = !wired && bundle_wire_admit(plain, request_generation);
    plain[0] = admitted;
    if (send_sealed_response(fd, &gcm, header, cipher, plain, 1, deadline) && admitted)
      bundle_wire_session(fd, &gcm, cipher, plain, request_generation);
  } else if (ok && mode == LAN_OTA_MODE) {
    const bool trusted = !wired && !atomic_load(&rtc_wire_busy) && ota_wire_trusted(request_generation);
    plain[0] = trusted;
    if (send_sealed_response(fd, &gcm, header, cipher, plain, 1, deadline) && trusted)
      ota_wire_session(fd, &gcm, cipher, plain, request_generation);
  } else if (ok && (mode == PET_LAN_CONTROL_COMPONENT || mode == PET_LAN_CONTROL_REBOOT)) {
    const uint8_t result = wired ? 0 : control_deliver(fd, mode, plain, size, request_generation, NULL, NULL, NULL);
    mbedtls_platform_zeroize(plain, size);
    plain[0] = result;
    send_sealed_response(fd, &gcm, header, cipher, plain, 1, esp_timer_get_time() + LAN_IO_US);
  } else if (ok && mode == PET_LAN_CONTROL_WIDGET_DATA) {
    cJSON *request = NULL;
    const size_t json_size = size - LAN_WIDGET_PREFIX_BYTES;
    plain[size] = 0;
    if (!wired && atomic_load(&control_trusted) && !atomic_load(&rtc_wire_busy) &&
        pet_json_source_text_limit_bytes(plain + LAN_WIDGET_PREFIX_BYTES, json_size, 3))
      request = cJSON_ParseWithLengthOpts((char *)plain + LAN_WIDGET_PREFIX_BYTES,
        json_size + 1, NULL, true);
    const uint8_t result = cJSON_IsObject(request)
      ? control_deliver(fd, mode, plain, size, request_generation, request, NULL, NULL) : 0;
    cJSON_Delete(request);
    mbedtls_platform_zeroize(plain, size);
    plain[0] = result;
    send_sealed_response(fd, &gcm, header, cipher, plain, 1, esp_timer_get_time() + LAN_IO_US);
  } else if (ok && mode == PET_LAN_CONTROL_APPEARANCE) {
    cJSON *request = NULL;
    plain[size] = 0;
    if (!wired && pet_json_source_text_limit_bytes(plain + 17, size - 17, 2))
      request = cJSON_ParseWithLengthOpts((char *)plain + 17, size - 16, NULL, true);
    const bool supported = cJSON_IsObject(request);
    // After decryption, the ciphertext buffer is available to hold the ACK.
    cipher[0] = 0;
    if (supported)
      control_deliver(fd, mode, plain, size, request_generation, request, NULL, (char *)cipher);
    cJSON_Delete(request);
    const size_t length = cipher[0] ? strlen((char *)cipher) : 1;
    memcpy(plain, cipher, length);
    send_sealed_response(fd, &gcm, header, cipher, plain, length, esp_timer_get_time() + LAN_IO_US);
  } else if (ok && mode == PET_LAN_CONTROL_KEYBOARD) {
    cJSON *request = NULL, *reply = NULL;
    plain[size] = 0;
    if (!wired && pet_json_source_text_limit_bytes(plain + 16, size - 16, 2))
      request = cJSON_ParseWithLengthOpts((char *)plain + 16, size - 16 + 1, NULL, true);
    if (cJSON_IsObject(request))
      control_deliver(fd, mode, plain, size, request_generation, request, &reply, NULL);
    mbedtls_platform_zeroize(plain, size);
    size_t length = 1;
    if (reply && cJSON_PrintPreallocated(reply, (char *)plain, LAN_KEYBOARD_REPLY_MAX, false))
      length = strlen((char *)plain);
    else plain[0] = 0;
    cJSON_Delete(request);
    cJSON_Delete(reply);
    send_sealed_response(fd, &gcm, header, cipher, plain, length, esp_timer_get_time() + LAN_IO_US);
  } else if (ok && mode == PET_LAN_CONTROL_PANEL) {
    const unsigned action = plain[16];
    cJSON *request = NULL, *reply = NULL;
    if ((action == PET_LAN_PANEL_WRITE_INPUT || action == PET_LAN_PANEL_REMOVE_COMPONENT || action == PET_LAN_PANEL_TEXT_PIXELS) && size > 17 &&
        pet_json_source_text_limit_bytes(plain + 17, size - 17, 4)) {
      plain[size] = 0;
      request = cJSON_ParseWithLengthOpts((const char *)plain + 17, size - 17 + 1, NULL, true);
    }
    const bool valid = action == PET_LAN_PANEL_WRITE_INPUT ? lan_panel_valid_input(request)
      : action == PET_LAN_PANEL_REMOVE_COMPONENT ? lan_panel_remove_valid(request)
      : action == PET_LAN_PANEL_TEXT_PIXELS ? size <= 1553 && lan_bubble_valid(request)
      : size == 17 && action >= PET_LAN_PANEL_READ_INPUT && action <= PET_LAN_PANEL_DISABLE_MICROPHONE;
    if (!wired && valid) control_deliver(fd, mode, plain, size, request_generation, request, &reply, NULL);
    mbedtls_platform_zeroize(plain, size);
    size_t length = 1;
    if (reply && cJSON_PrintPreallocated(reply, (char *)plain, LAN_PANEL_REPLY_MAX, false))
      length = strlen((const char *)plain);
    else plain[0] = 0;
    cJSON_Delete(request);
    cJSON_Delete(reply);
    send_sealed_response(fd, &gcm, header, cipher, plain, length, esp_timer_get_time() + LAN_IO_US);
  } else if (ok && mode == LAN_STATUS_MODE) {
    if (!size || plain[0] == 1)
      send_device_status(fd, &gcm, header, cipher, plain, deadline, size == 1);
  } else if (ok && (mode == LAN_VOLUME_REQUEST || mode == LAN_VOLUME_QUERY)) {
    plain[0] = volume_result;
    send_sealed_response(fd, &gcm, header, cipher, plain, 1, deadline);
  } else if (ok) {
    nonce[0] = 1;
    if (mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 0, nonce, sizeof(nonce),
        header, sizeof(header), plain, cipher, LAN_TAG_SIZE, cipher) == 0)
      socket_io(fd, cipher, LAN_TAG_SIZE, true, deadline);
  }
  cJSON_Delete(update.root);
  if (mode == LAN_PRIVATE_FEED_MODE) mbedtls_platform_zeroize(plain, size);
  mbedtls_gcm_free(&gcm);
  mbedtls_platform_zeroize(master, sizeof(master));
}

static int listener_for(uint32_t ip) {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(LAN_PORT),
                               .sin_addr.s_addr = ip };
  const int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (bind(fd, (struct sockaddr *)&local, sizeof(local)) ||
      listen(fd, 4) || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void lan_worker(void *unused) {
  (void)unused;
  uint8_t *buffers = heap_caps_malloc(2 * LAN_MAX_BODY + LAN_TAG_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  while (!buffers) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    buffers = heap_caps_malloc(2 * LAN_MAX_BODY + LAN_TAG_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  nvs_handle_t handle;
  esp_err_t loaded = nvs_open("p4_agent_lan", NVS_READONLY, &handle);
  if (loaded == ESP_OK) {
    size_t size = sizeof(paired_key);
    loaded = nvs_get_blob(handle, "key", paired_key, &size);
    paired = loaded == ESP_OK && size == sizeof(paired_key);
    state_known = paired || loaded == ESP_ERR_NVS_NOT_FOUND;
    atomic_store(&control_trusted, paired && trust_storage(handle, false));
    nvs_close(handle);
  } else state_known = loaded == ESP_ERR_NVS_NOT_FOUND;
  failed = !state_known;
  xSemaphoreGive(lock);

  uint32_t bound_ip = 0;
  int listener = -1;
  int discovery=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  if(discovery>=0){struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(29462),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(discovery,(struct sockaddr *)&a,sizeof(a)) || fcntl(discovery,F_SETFL,O_NONBLOCK)<0){close(discovery);discovery=-1;}}
  // Match the USB board identity: ESP32-P4 uses a hosted Wi-Fi coprocessor.
  uint8_t mac[6]={0};const bool identity_ok=esp_read_mac(mac,ESP_MAC_BASE)==ESP_OK;
  for (;;) {
    esp_netif_ip_info_t info = {0};
    if (pet_device_wifi_online()) {
      esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if (sta) esp_netif_get_ip_info(sta, &info);
    }
    cJSON_Delete(atomic_exchange(&retired, NULL));
    xSemaphoreTake(lock, portMAX_DELAY);
    if (atomic_exchange(&forget_pending, false)) {
      atomic_store(&control_trusted, false);
      pet_lan_press_clear(&realtime_press);
      trust_challenge.expires = 0;
      // Do not reopen enrollment if persistent removal fails.
      failed = !store_key(NULL);
      if (!failed) {
        paired = false;
        state_known = true;
        ++generation;
      client_seen_ms = 0;
        mbedtls_platform_zeroize(paired_key, sizeof(paired_key));
        volume_control = (pet_lan_volume_t){0};
        cJSON_Delete(mailbox.root);
        mailbox.root = NULL;
      }
    }
    struct in_addr ip = { .s_addr = info.ip.addr };
    refresh_volume();
    if (!info.ip.addr || !inet_ntop(AF_INET, &ip, address, sizeof(address))) address[0] = 0;
    const uint32_t listen_ip = state_known ? info.ip.addr : 0;
    const bool discovery_ready = pet_p4_lan_can_pair(info.ip.addr != 0, state_known, paired, pairing || atomic_load(&forget_pending));
    const bool discovery_paired = paired;
    xSemaphoreGive(lock);

    if (listen_ip != bound_ip || (listen_ip && listener < 0)) {
      if (listener >= 0) close(listener);
      bound_ip = listen_ip;
      listener = bound_ip ? listener_for(bound_ip) : -1;
    }

    if(discovery>=0 && info.ip.addr && identity_ok){
      char query[80];struct sockaddr_in from;socklen_t n=sizeof(from);
      int got=recvfrom(discovery,query,sizeof(query)-1,0,(struct sockaddr *)&from,&n);
      if(got>0 && info.netmask.addr && ((from.sin_addr.s_addr^info.ip.addr)&info.netmask.addr)==0){
        query[got]=0;const char *discovery_nonce=pet_p4_lan_discovery_nonce(query,(size_t)got);
        if(discovery_nonce){char answer[256];int len=snprintf(answer,sizeof(answer),"{\"protocol\":2,\"pairing\":\"auto-unpaired-v1\",\"nonce\":\"%s\",\"id\":\"p4-%02x%02x%02x%02x%02x%02x\",\"ready\":%s,\"paired\":%s}",discovery_nonce,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],discovery_ready?"true":"false",discovery_paired?"true":"false");
          if(len>0 && len<(int)sizeof(answer))sendto(discovery,answer,len,0,(struct sockaddr *)&from,n);}
      }
    }
    if (listener >= 0) {
      struct sockaddr_in peer;
      socklen_t size = sizeof(peer);
      const int fd = accept(listener, (struct sockaddr *)&peer, &size);
      if (fd >= 0) {
        bool handed_off = false;
        if (info.netmask.addr && ((peer.sin_addr.s_addr ^ bound_ip) & info.netmask.addr) == 0 &&
            fcntl(fd, F_SETFL, O_NONBLOCK) == 0)
          handle_peer(fd, buffers, buffers + LAN_MAX_BODY + LAN_TAG_SIZE, &handed_off);
        if (!handed_off) close(fd);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

void pet_p4_lan_start(void) {
  if (worker || !pet_p4_is_wifi6_hardware() || !pet_device_wifi_online()) return;
  if (!lock) lock = xSemaphoreCreateMutex();
  if (lock) xTaskCreate(lan_worker, "pet_agent_lan", 6144, NULL, tskIDLE_PRIORITY + 1, &worker);
}

void pet_p4_lan_process(pet_p4_runtime_state_t *state, uint64_t now_ms) {
  static bool have_status, have_speech;
  static uint8_t last_status[32], last_speech[32];
  static char last_source[PET_P4_AGENT_MAX];
  static uint64_t last_seen, last_pixel_manifest;
  static uint32_t last_generation, last_pixel_generation;
  const bool usb = pet_p4_state_usb_client_connected(state, now_ms);
  atomic_store(&usb_client_active, usb);
  if (usb || state->approval_active) pet_p4_lan_voice_cancel();
  if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) return;
  const lan_update_t update = mailbox;
  mailbox.root = NULL;
  const uint32_t current_generation = generation;
  const bool enabled = paired && !atomic_load(&forget_pending);
  const bool online = pet_device_wifi_online();
  rtc_hud_process(state, now_ms, enabled && online && !usb && atomic_load(&control_trusted), current_generation);
  if (!enabled || !online || usb) client_seen_ms = 0;
  pet_lan_press_refresh(&realtime_press, now_ms, enabled && online && !usb && atomic_load(&control_trusted));
  const bool recently_seen = client_seen_ms &&
    (now_ms < client_seen_ms || now_ms - client_seen_ms < LAN_LEASE_MS);
  const bool client_connected = !usb && enabled && online && (recently_seen || atomic_load(&rtc_wire_busy));
  /* Retire only this transport: USB may already have installed its own owner. */
  if (!enabled || !online || usb || !recently_seen || last_pixel_generation != current_generation
      || now_ms - last_pixel_manifest >= LAN_LEASE_MS || !atomic_load(&control_trusted))
    pet_bubble_pixels_retire_route(PET_BUBBLE_LAN_ROUTE);
  xSemaphoreGive(lock);

  if (state->lan_client_connected != client_connected) {
    state->lan_client_connected = client_connected;
    state->last_update_ms += 1;
  }
  const bool expired = last_seen && (now_ms - last_seen >= LAN_LEASE_MS ||
    !enabled || last_generation != current_generation || !online);
  if (last_seen && (usb || expired)) {
    if (!usb) pet_p4_lan_expire(state);
    last_seen = 0;
    last_source[0] = 0;
    have_status = have_speech = false;
  }
  cJSON *snapshot = update.root;
  if (!snapshot) return;
  const uint64_t age = update.received > now_ms ? 0 : now_ms - update.received;
  if (!usb && enabled && online && age < LAN_LEASE_MS &&
      !state->asset_transfer_active && !pet_p4_ota_transfer_active()) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (current_generation == generation && volume_available() && atomic_load(&control_trusted)
        && !atomic_load(&usb_client_active)) {
      pet_bubble_pixels_desire(snapshot, PET_BUBBLE_LAN_ROUTE, "lan", now_ms);
      last_pixel_manifest = now_ms;
      last_pixel_generation = current_generation;
    }
    xSemaphoreGive(lock);
    const char *source = cJSON_GetObjectItemCaseSensitive(snapshot, "source")->valuestring;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(snapshot, "status");
    const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(snapshot, "sessions");
    const cJSON *speech = cJSON_GetObjectItemCaseSensitive(snapshot, "speech");
    /* A realtime HUD publishes pixels on this control lane while the existing
     * audio connection owns its text. This envelope leaves Agent state alone. */
    if (!status && !sessions && !speech) goto retire_snapshot;
    const bool source_changed = last_source[0] && strcmp(last_source, source);
    if (source_changed || (!status && have_status)) {
      pet_p4_lan_expire(state);
      have_status = false;
    }
    if (source_changed) have_speech = false;
    const bool new_status = status && (!have_status || memcmp(last_status, update.status_hash, 32));
    const bool new_speech = speech && (!have_speech || memcmp(last_speech, update.speech_hash, 32));
    const bool expired_speech = have_speech && !speech;
    const cJSON empty_speech = { .type = cJSON_Object };
    const cJSON *speech_update = expired_speech ? &empty_speech : new_speech ? speech : NULL;
    if (new_status || speech_update || sessions)
      pet_p4_lan_apply(state, source, new_status ? status : NULL, speech_update, sessions);
    if (expired_speech) have_speech = false;
    if (new_status) {
      memcpy(last_status, update.status_hash, sizeof(last_status));
      have_status = true;
    }
    if (new_speech) {
      memcpy(last_speech, update.speech_hash, sizeof(last_speech));
      have_speech = true;
    }
    snprintf(last_source, sizeof(last_source), "%s", source);
    last_seen = now_ms;
    last_generation = current_generation;
  }
retire_snapshot:
  /* The worker normally collects the consumed JSON before its next accept. */
  cJSON_Delete(atomic_exchange(&retired, snapshot));
}
#endif

#if PET_P4_SYSTEM_WIFI
/* Called under the state lane; never wait on network IO while holding it.
 * Without a live desktop USB session realtime input belongs exclusively to LAN,
 * even if no LAN desktop is listening. Never leak it into a diagnostic serial reader. */
bool pet_p4_lan_realtime_press(const pet_p4_runtime_state_t *state, uint64_t pressed, uint64_t now) {
  if (pet_p4_state_usb_client_connected(state, now)) return false;
  if (!lock || (xSemaphoreTake(lock, 0) != pdTRUE && xSemaphoreTake(lock, 1) != pdTRUE)) return true;
  const bool allowed = paired && !atomic_load(&forget_pending) && pet_device_wifi_online() &&
    atomic_load(&control_trusted) && !atomic_load(&usb_client_active);
  pet_lan_press_refresh(&realtime_press, now, allowed);
  uint8_t context = !strcmp(state->screen_page, "main") ? 0 :
    !strcmp(state->screen_page, "app") ? 1 : !strcmp(state->screen_page, "components") ? 2 : 255;
  if (allowed) (void)pet_lan_press_push(&realtime_press, now, pressed, context);
  xSemaphoreGive(lock);
  return true;
}
#endif
