/*
 * [Input] firmware update JSON topics, inactive ESP-IDF OTA partition, and runtime
 *         readiness from the LCD/render initialization path.
 * [Output] 4KB ACK-gated A/B image writes, request-correlated resumable status,
 *          SHA-256 verification, boot-slot switching, delayed validity
 *          confirmation, automatic rollback and chip-revision anti-misflash checks.
 * [Pos] ESP32-P4 firmware update node in firmware/main
 * [Sync] Keep firmware/protocol.md and .folder.md aligned with the public USB protocol.
 *        See firmware/docs/wireless-ota-adapter.md for the typed wireless contract.
 */

// This control path is paced by flash writes and transport acknowledgements.
#pragma GCC optimize ("Oz")
#include "pet_p4_ota.h"
#include "pet_p4_wireless_ota.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "pet_p4_image_compat.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_device_wifi.h"
#include "pet_p4_boot_trace.h"
#if PET_P4_OTA_METADATA_CACHED
#include "esp_attr.h"
#endif

#define PET_P4_OTA_TRANSFER_ID_MAX 64
#define PET_P4_OTA_SHA256_HEX_LEN 64
#define PET_P4_OTA_DECODED_CHUNK_MAX PET_P4_WIRELESS_OTA_CHUNK_BYTES
#define PET_P4_OTA_VALIDATE_STABLE_MS 8000ULL
#define PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS 1000ULL
#define PET_P4_OTA_VALIDATE_MIN_SAMPLES 20U
#define PET_P4_OTA_FAILED_BOOT_RESTART_MS 20000ULL
#define PET_P4_OTA_RESTART_DELAY_MS 5000ULL
#define PET_P4_OTA_TRANSFER_IDLE_TIMEOUT_MS 30000ULL

typedef struct {
  bool valid;
  bool wireless;
  char transfer_id[PET_P4_OTA_TRANSFER_ID_MAX];
  char target_partition[17];
  char version[sizeof(((esp_app_desc_t *) 0)->version)];
  char project_name[sizeof(((esp_app_desc_t *) 0)->project_name)];
  char sha256[PET_P4_OTA_SHA256_HEX_LEN + 1];
  unsigned long long bytes;
  unsigned int next_sequence;
} pet_p4_ota_commit_result_t;

static const char *TAG = "pet-p4-ota";
static pet_p4_runtime_state_t *g_state;
static esp_ota_handle_t g_ota_handle;
static const esp_partition_t *g_update_partition;
static char g_transfer_id[PET_P4_OTA_TRANSFER_ID_MAX];
static char g_expected_sha256[PET_P4_OTA_SHA256_HEX_LEN + 1];
static unsigned long long g_expected_size;
static unsigned long long g_received_size;
static unsigned int g_next_sequence;
static unsigned int g_last_chunk_sequence;
static size_t g_last_chunk_size;
static bool g_last_chunk_valid;
static mbedtls_sha256_context g_sha256;
static bool g_sha256_initialized;
static bool g_active;
static bool g_runtime_ready;
static bool g_pending_verify;
static unsigned int g_runtime_healthy_samples;
static unsigned long long g_runtime_first_healthy_ms;
static unsigned long long g_runtime_last_healthy_ms;
static unsigned long long g_initialized_ms;
static unsigned long long g_restart_at_ms;
static unsigned long long g_last_activity_ms;
static SemaphoreHandle_t g_ota_mutex;
static bool g_initialized;
static pet_p4_ota_commit_result_t g_commit_result;
#if PET_P4_SYSTEM_WIFI
static uint8_t g_wireless_owner[PET_P4_WIRELESS_OTA_OWNER_BYTES];
static uint8_t g_wireless_installed_digest[PET_P4_WIRELESS_OTA_DIGEST_BYTES];
#endif

#if PET_P4_OTA_METADATA_CACHED
/* Descriptor pointers are native boot data, stable until reboot, and are
 * only read/written under the existing OTA mutex. A failed set-boot result
 * invalidates the boot observation instead of publishing a stale success. */
static const esp_partition_t *g_observed_running,*g_observed_boot,*g_observed_next;
/* Hot diagnostics need only these two scalar values. No SDK, allocation,
 * external-memory pointer, log, callback or filesystem call under this mux. */
static DRAM_ATTR portMUX_TYPE g_image_observation_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR esp_ota_img_states_t g_observed_image_state=ESP_OTA_IMG_UNDEFINED;
static DRAM_ATTR esp_err_t g_observed_image_error=ESP_ERR_INVALID_STATE;
static void observe_image_state(esp_err_t error,esp_ota_img_states_t state){
  portENTER_CRITICAL(&g_image_observation_mux);
  if(error==ESP_OK)g_observed_image_state=state;
  g_observed_image_error=error;
  portEXIT_CRITICAL(&g_image_observation_mux);
}
esp_err_t pet_p4_ota_observed_image_state(esp_ota_img_states_t *out){
  if(!out)return ESP_ERR_INVALID_ARG;
  esp_ota_img_states_t state;esp_err_t error;
  portENTER_CRITICAL(&g_image_observation_mux);
  state=g_observed_image_state;error=g_observed_image_error;
  portEXIT_CRITICAL(&g_image_observation_mux);
  *out=state;return error;
}
static void observe_boot_result(esp_err_t error,const esp_partition_t *target){
  /* Caller still owns the existing OTA mutex after the original SDK call. */
  g_observed_boot=error==ESP_OK?target:NULL;
}
#endif

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
  return cJSON_IsString(item) ? item->valuestring : "";
}

static unsigned long long json_u64(
  const cJSON *object,
  const char *key,
  unsigned long long fallback
) {
  const cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
  double integer = 0;
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)
      || item->valuedouble < 0
      || item->valuedouble >= 18446744073709551616.0
      || modf(item->valuedouble, &integer) != 0.0) {
    return fallback;
  }
  return (unsigned long long) integer;
}

static void send_topic(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *topic,
  cJSON *payload
) {
  cJSON *root = cJSON_CreateObject();
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", topic);
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static const char *ota_state_name(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default: return "undefined";
  }
}

static void add_partition(cJSON *payload, const char *key, const esp_partition_t *partition) {
  cJSON *item = cJSON_CreateObject();
  if (!item) return;
  if (partition) {
    cJSON_AddStringToObject(item, "label", partition->label);
    cJSON_AddNumberToObject(item, "address", partition->address);
    cJSON_AddNumberToObject(item, "size", partition->size);
    cJSON_AddNumberToObject(item, "subtype", partition->subtype);
  }
  cJSON_AddItemToObject(payload, key, item);
}

static void send_status_locked(
  const cJSON *request,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
#if PET_P4_OTA_METADATA_CACHED
  const esp_partition_t *running=g_observed_running;
  const esp_partition_t *boot=g_observed_boot;
  const esp_partition_t *next=g_observed_next;
  const esp_app_desc_t *app=esp_app_get_description();
  esp_ota_img_states_t image_state=ESP_OTA_IMG_UNDEFINED;
  esp_err_t state_err=pet_p4_ota_observed_image_state(&image_state);
#else
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  const esp_app_desc_t *app = esp_app_get_description();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t state_err = running
    ? esp_ota_get_state_partition(running, &image_state)
    : ESP_ERR_NOT_FOUND;
#endif
  cJSON *payload = cJSON_CreateObject();
  const char *request_id = json_string(request, "requestId");
  if (request_id[0]) cJSON_AddStringToObject(payload, "requestId", request_id);
  cJSON_AddBoolToObject(payload, "supported", running && next && running != next);
  cJSON_AddBoolToObject(payload, "active", g_active);
  cJSON_AddBoolToObject(payload, "pendingVerify", g_pending_verify);
  cJSON_AddBoolToObject(payload, "runtimeReady", g_runtime_ready);
#if PET_P4_OTA_METADATA_CACHED
  cJSON_AddBoolToObject(payload,"metadataCached",true);
  cJSON_AddBoolToObject(payload,"bootPartitionAvailable",boot!=NULL);
#endif
  cJSON_AddStringToObject(payload, "version", app ? app->version : "unknown");
  cJSON_AddStringToObject(payload, "projectName", app ? app->project_name : "unknown");
  cJSON_AddStringToObject(
    payload,
    "imageState",
    state_err == ESP_OK ? ota_state_name(image_state) : "unavailable"
  );
  add_partition(payload, "runningPartition", running);
  add_partition(payload, "bootPartition", boot);
  add_partition(payload, "nextPartition", next);
  if (g_active) {
    cJSON_AddStringToObject(payload, "transferId", g_transfer_id);
    cJSON_AddNumberToObject(payload, "receivedBytes", (double) g_received_size);
    cJSON_AddNumberToObject(payload, "expectedBytes", (double) g_expected_size);
    cJSON_AddNumberToObject(payload, "nextSequence", g_next_sequence);
  }
  send_topic(send_line, ctx, "firmware/status", payload);
}

static void send_ack_details(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  bool ok,
  const char *error,
  bool include_chunk,
  unsigned long long sequence,
  size_t chunk_bytes,
  bool duplicate
) {
  cJSON *payload = cJSON_CreateObject();
  if (!payload) return;
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  cJSON_AddBoolToObject(payload, "ok", ok);
  cJSON_AddNumberToObject(payload, "receivedBytes", (double) g_received_size);
  cJSON_AddNumberToObject(payload, "expectedBytes", (double) g_expected_size);
  cJSON_AddNumberToObject(payload, "nextSequence", g_next_sequence);
  if (include_chunk) {
    cJSON_AddNumberToObject(payload, "seq", (double) sequence);
    cJSON_AddNumberToObject(payload, "chunkBytes", (double) chunk_bytes);
    cJSON_AddBoolToObject(payload, "duplicate", duplicate);
  }
  if (g_update_partition) cJSON_AddStringToObject(payload, "targetPartition", g_update_partition->label);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  send_topic(send_line, ctx, "firmware/ack", payload);
}

static void send_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  bool ok,
  const char *error
) {
  send_ack_details(send_line, ctx, transfer_id, phase, ok, error, false, 0, 0, false);
}

static void send_chunk_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  bool ok,
  const char *error,
  unsigned long long sequence,
  size_t chunk_bytes,
  bool duplicate
) {
  send_ack_details(
    send_line,
    ctx,
    transfer_id,
    "chunk",
    ok,
    error,
    true,
    sequence,
    chunk_bytes,
    duplicate
  );
}

static void send_commit_ack(pet_p4_send_line_fn send_line, void *ctx) {
  cJSON *ack = cJSON_CreateObject();
  if (!ack || !g_commit_result.valid) {
    cJSON_Delete(ack);
    return;
  }
  cJSON_AddStringToObject(ack, "transferId", g_commit_result.transfer_id);
  cJSON_AddStringToObject(ack, "phase", "commit");
  cJSON_AddBoolToObject(ack, "ok", true);
  cJSON_AddBoolToObject(ack, "pendingReboot", true);
  cJSON_AddStringToObject(ack, "targetPartition", g_commit_result.target_partition);
  cJSON_AddStringToObject(ack, "version", g_commit_result.version);
  cJSON_AddStringToObject(ack, "projectName", g_commit_result.project_name);
  cJSON_AddStringToObject(ack, "sha256", g_commit_result.sha256);
  cJSON_AddNumberToObject(ack, "bytes", (double) g_commit_result.bytes);
  cJSON_AddNumberToObject(ack, "receivedBytes", (double) g_commit_result.bytes);
  cJSON_AddNumberToObject(ack, "expectedBytes", (double) g_commit_result.bytes);
  cJSON_AddNumberToObject(ack, "nextSequence", g_commit_result.next_sequence);
  send_topic(send_line, ctx, "firmware/ack", ack);
}

static void clear_transfer(bool abort_write) {
  if (g_active && abort_write && g_ota_handle) {
    (void) esp_ota_abort(g_ota_handle);
  }
  if (g_sha256_initialized) {
    mbedtls_sha256_free(&g_sha256);
    g_sha256_initialized = false;
  }
  g_ota_handle = 0;
  g_update_partition = NULL;
  g_transfer_id[0] = '\0';
#if PET_P4_SYSTEM_WIFI
  memset(g_wireless_owner, 0, sizeof(g_wireless_owner));
  memset(g_wireless_installed_digest, 0, sizeof(g_wireless_installed_digest));
#endif
  g_expected_sha256[0] = '\0';
  g_expected_size = 0;
  g_received_size = 0;
  g_next_sequence = 0;
  g_last_chunk_sequence = 0;
  g_last_chunk_size = 0;
  g_last_chunk_valid = false;
  g_last_activity_ms = 0;
  g_active = false;
}

static bool transfer_matches(const cJSON *payload) {
  const char *transfer_id = json_string(payload, "transferId");
  return g_active && transfer_id[0] && strcmp(transfer_id, g_transfer_id) == 0;
}

static bool sha256_text_valid(const char *value) {
  if (!value || strlen(value) != PET_P4_OTA_SHA256_HEX_LEN) return false;
  for (size_t i = 0; value[i]; i += 1) {
    if (!((value[i] >= '0' && value[i] <= '9')
          || (value[i] >= 'a' && value[i] <= 'f')
          || (value[i] >= 'A' && value[i] <= 'F'))) {
      return false;
    }
  }
  return true;
}

static void digest_hex(const unsigned char digest[32], char output[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i += 1) {
    output[i * 2] = digits[digest[i] >> 4];
    output[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  output[64] = '\0';
}

static const char *begin_transfer(const char *transfer_id, const char *sha256,
                                  unsigned long long size, bool check_revision,
                                  unsigned long long min, unsigned long long max) {
  if (g_restart_at_ms) {
    return "device is rebooting into committed firmware";
  }
  if (pet_p4_diagnostics_reboot_pending()) {
    return "device reboot is already scheduled";
  }
  if (g_pending_verify) {
    return "running firmware is still pending validation";
  }
  if (g_state && g_state->asset_transfer_active) {
    return "appearance asset transfer is active";
  }
  if (g_active) {
    return "another firmware transfer is active";
  }
  g_update_partition = esp_ota_get_next_update_partition(NULL);
  if (!transfer_id[0] || strlen(transfer_id) >= sizeof(g_transfer_id)
      || !sha256_text_valid(sha256) || size == 0 || !g_update_partition) {
    clear_transfer(false);
    return "invalid metadata or no inactive OTA partition";
  }
  if (size > g_update_partition->size) {
    clear_transfer(false);
    return "firmware image exceeds OTA partition";
  }
  if (check_revision) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    if (min > UINT16_MAX || max > UINT16_MAX
        || !pet_p4_revision_range_valid((uint16_t) min, (uint16_t) max)
        || chip_info.revision < min || chip_info.revision > max) {
      clear_transfer(false);
      return "firmware chip revision mismatch";
    }
  }
  esp_err_t err = esp_ota_begin(g_update_partition, (size_t) size, &g_ota_handle);
  if (err != ESP_OK) {
    clear_transfer(false);
    return esp_err_to_name(err);
  }
  mbedtls_sha256_init(&g_sha256);
  g_sha256_initialized = true;
  if (mbedtls_sha256_starts(&g_sha256, false) != 0) {
    snprintf(g_transfer_id, sizeof(g_transfer_id), "%s", transfer_id);
    g_active = true;
    clear_transfer(true);
    return "SHA-256 initialization failed";
  }
  snprintf(g_transfer_id, sizeof(g_transfer_id), "%s", transfer_id);
  snprintf(g_expected_sha256, sizeof(g_expected_sha256), "%s", sha256);
  for (size_t i = 0; g_expected_sha256[i]; i += 1) {
    if (g_expected_sha256[i] >= 'A' && g_expected_sha256[i] <= 'F') {
      g_expected_sha256[i] = (char) (g_expected_sha256[i] - 'A' + 'a');
    }
  }
  g_expected_size = size;
  g_received_size = 0;
  g_next_sequence = 0;
  g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  g_active = true;
  ESP_LOGI(TAG, "OTA begin transfer=%s target=%s bytes=%llu", g_transfer_id,
           g_update_partition->label, g_expected_size);
  return NULL;
}

static void handle_begin(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  const char *id = json_string(payload, "transferId");
  bool has_revision = cJSON_GetObjectItemCaseSensitive(payload, "chipRevisionMin")
    || cJSON_GetObjectItemCaseSensitive(payload, "chipRevisionMax");
  const char *error = begin_transfer(id, json_string(payload, "sha256"),
    json_u64(payload, "size", 0), has_revision,
    json_u64(payload, "chipRevisionMin", UINT64_MAX),
    json_u64(payload, "chipRevisionMax", UINT64_MAX));
  send_ack(send_line, ctx, id, "begin", error == NULL, error);
}

static const char *write_chunk(unsigned int sequence, const uint8_t *data, size_t size) {
  if (g_received_size == 0) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    if (!pet_p4_image_header_matches(data, size, chip_info.revision)) {
      clear_transfer(true);
      return "firmware chip revision mismatch";
    }
  }
  esp_err_t error = esp_ota_write(g_ota_handle, data, size);
  if (error != ESP_OK || mbedtls_sha256_update(&g_sha256, data, size) != 0) {
    clear_transfer(true);
    return error != ESP_OK ? esp_err_to_name(error) : "SHA-256 update failed";
  }
  g_received_size += size;
  g_last_chunk_sequence = sequence;
  g_last_chunk_size = size;
  g_last_chunk_valid = true;
  g_next_sequence++;
  return NULL;
}

static void handle_chunk(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  unsigned long long requested_sequence = json_u64(payload, "seq", ULLONG_MAX);
  unsigned int sequence = requested_sequence <= UINT_MAX
    ? (unsigned int) requested_sequence
    : UINT_MAX;
  unsigned long long decoded_size = json_u64(payload, "decodedSize", 0);
  const char *encoded = json_string(payload, "data");
  const char *request_transfer_id = json_string(payload, "transferId");
  size_t encoded_len = strlen(encoded);
  size_t actual_size = 0;
  unsigned char *decoded;
  if (!transfer_matches(payload)) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware transferId mismatch",
      requested_sequence,
      0,
      false
    );
    return;
  }
  g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  if (requested_sequence > UINT_MAX) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware chunk sequence is invalid",
      requested_sequence,
      0,
      false
    );
    return;
  }
  if (g_last_chunk_valid && sequence == g_last_chunk_sequence
      && sequence + 1 == g_next_sequence) {
    if (decoded_size != g_last_chunk_size) {
      send_chunk_ack(
        send_line,
        ctx,
        request_transfer_id,
        false,
        "duplicate firmware chunk size mismatch",
        sequence,
        0,
        true
      );
      return;
    }
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      true,
      NULL,
      sequence,
      g_last_chunk_size,
      true
    );
    return;
  }
  if (sequence != g_next_sequence) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware chunk sequence mismatch",
      sequence,
      0,
      false
    );
    return;
  }
  if (!encoded_len || !decoded_size || decoded_size > PET_P4_OTA_DECODED_CHUNK_MAX
      || g_received_size + decoded_size > g_expected_size) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "invalid firmware chunk size",
      sequence,
      0,
      false
    );
    return;
  }
  decoded = (unsigned char *) malloc((size_t) decoded_size);
  if (!decoded) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "out of memory decoding firmware chunk",
      sequence,
      0,
      false
    );
    return;
  }
  int decode_result = mbedtls_base64_decode(
    decoded,
    (size_t) decoded_size,
    &actual_size,
    (const unsigned char *) encoded,
    encoded_len
  );
  if (decode_result != 0 || actual_size != decoded_size) {
    free(decoded);
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware chunk base64 mismatch",
      sequence,
      0,
      false
    );
    return;
  }
  const char *error = write_chunk(sequence, decoded, actual_size);
  free(decoded);
  if (error) {
    send_chunk_ack(send_line, ctx, request_transfer_id, false, error, sequence, 0, false);
    return;
  }
  send_chunk_ack(
    send_line,
    ctx,
    request_transfer_id,
    true,
    NULL,
    sequence,
    actual_size,
    false
  );
}

static const char *commit_transfer(bool wireless, uint8_t *installed_digest) {
#if !PET_P4_SYSTEM_WIFI
  (void) installed_digest;
#endif
  unsigned char digest[32];
  char actual_sha256[65];
  esp_app_desc_t image_desc;
  const esp_app_desc_t *running_desc = esp_app_get_description();
  g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  if (g_received_size != g_expected_size) {
    return "firmware byte total mismatch";
  }
  if (mbedtls_sha256_finish(&g_sha256, digest) != 0) {
    clear_transfer(true);
    return "SHA-256 finalize failed";
  }
  mbedtls_sha256_free(&g_sha256);
  g_sha256_initialized = false;
  digest_hex(digest, actual_sha256);
  if (strcmp(actual_sha256, g_expected_sha256) != 0) {
    clear_transfer(true);
    return "firmware SHA-256 mismatch";
  }
  esp_err_t end_err = esp_ota_end(g_ota_handle);
  g_ota_handle = 0;
  if (end_err != ESP_OK) {
    clear_transfer(false);
    return esp_err_to_name(end_err);
  }
  esp_err_t desc_err = esp_ota_get_partition_description(g_update_partition, &image_desc);
  if (desc_err != ESP_OK) {
    clear_transfer(false);
    return "firmware image description is invalid";
  }
  if (!running_desc || !running_desc->project_name[0]
      || strcmp(image_desc.project_name, running_desc->project_name) != 0) {
    clear_transfer(false);
    return "firmware project identity mismatch";
  }
#if PET_P4_SYSTEM_WIFI
  if (wireless) {
    const size_t n = strnlen(image_desc.version, sizeof(image_desc.version));
    if (n < 6 || n == sizeof(image_desc.version) || memcmp(image_desc.version + n - 6, "-wifi6", 6)) {
      clear_transfer(false);
      return "wireless firmware requires a Wi-Fi 6 image";
    }
  }
  // Read the validated target's native digest before changing boot metadata.
  if (wireless && esp_partition_get_sha256(g_update_partition, installed_digest) != ESP_OK) {
    clear_transfer(false);
    return "firmware partition digest unavailable";
  }
  if (wireless && memcmp(installed_digest, g_wireless_installed_digest,
                         sizeof(g_wireless_installed_digest)) != 0) {
    clear_transfer(false);
    return "firmware partition digest mismatch";
  }
#endif
  esp_err_t boot_err = esp_ota_set_boot_partition(g_update_partition);
#if PET_P4_OTA_METADATA_CACHED
  observe_boot_result(boot_err,g_update_partition);
#endif
  if (boot_err != ESP_OK) {
    clear_transfer(false);
    return esp_err_to_name(boot_err);
  }
  memset(&g_commit_result, 0, sizeof(g_commit_result));
  g_commit_result.valid = true;
  g_commit_result.wireless = wireless;
  snprintf(g_commit_result.transfer_id, sizeof(g_commit_result.transfer_id), "%s", g_transfer_id);
  snprintf(g_commit_result.target_partition, sizeof(g_commit_result.target_partition), "%s",
           g_update_partition->label);
  snprintf(g_commit_result.version, sizeof(g_commit_result.version), "%s", image_desc.version);
  snprintf(g_commit_result.project_name, sizeof(g_commit_result.project_name), "%s",
           image_desc.project_name);
  snprintf(g_commit_result.sha256, sizeof(g_commit_result.sha256), "%s", actual_sha256);
  g_commit_result.bytes = g_received_size;
  g_commit_result.next_sequence = g_next_sequence;
  ESP_LOGI(TAG, "OTA verified transfer=%s target=%s version=%s; scheduling reboot",
           g_transfer_id, g_update_partition->label, image_desc.version);
  clear_transfer(false);
  g_restart_at_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL)
    + PET_P4_OTA_RESTART_DELAY_MS;
  return NULL;
}

static void handle_commit(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  const char *id = json_string(payload, "transferId");
  if (!g_active && g_commit_result.valid && !g_commit_result.wireless
      && id[0] && strcmp(id, g_commit_result.transfer_id) == 0) {
    send_commit_ack(send_line, ctx);
    return;
  }
  const char *error = transfer_matches(payload)
    ? commit_transfer(false, NULL) : "firmware transferId mismatch";
  if (error) send_ack(send_line, ctx, id, "commit", false, error);
  else send_commit_ack(send_line, ctx);
}

static void handle_abort(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  const char *request_transfer_id = json_string(payload, "transferId");
  if (!g_active) {
    send_ack(send_line, ctx, request_transfer_id, "abort", true, NULL);
    return;
  }
  if (!transfer_matches(payload)) {
    send_ack(send_line, ctx, request_transfer_id, "abort", false, "firmware transferId mismatch");
    return;
  }
  send_ack(send_line, ctx, g_transfer_id, "abort", true, NULL);
  clear_transfer(true);
}

esp_err_t pet_p4_ota_init(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (!g_ota_mutex) g_ota_mutex = xSemaphoreCreateMutex();
  if (!g_ota_mutex) return ESP_ERR_NO_MEM;
  if (xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
  g_state = state;
  memset(&g_commit_result, 0, sizeof(g_commit_result));
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t err = running
    ? esp_ota_get_state_partition(running, &image_state)
    : ESP_ERR_NOT_FOUND;
#if PET_P4_OTA_METADATA_CACHED
  g_observed_running=running;
  g_observed_boot=esp_ota_get_boot_partition();
  g_observed_next=esp_ota_get_next_update_partition(NULL);
  observe_image_state(err,image_state);
#endif
  g_pending_verify = err == ESP_OK && image_state == ESP_OTA_IMG_PENDING_VERIFY;
#if PET_P4_BOOT_TRACE_ENABLE
  if(err==ESP_OK&&image_state==ESP_OTA_IMG_VALID)(void)pet_boot_trace_mark(PET_BOOT_IMAGE_VALID);
#endif
  g_initialized_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  ESP_LOGI(TAG, "running partition=%s state=%s rollback=%s",
           running ? running->label : "none",
           err == ESP_OK ? ota_state_name(image_state) : "unavailable",
           g_pending_verify ? "pending" : "settled");
  bool supported = running &&
#if PET_P4_OTA_METADATA_CACHED
    g_observed_next;
#else
    esp_ota_get_next_update_partition(NULL);
#endif
  g_initialized = true;
  xSemaphoreGive(g_ota_mutex);
  pet_p4_ota_send_status(send_line, ctx);
  return supported ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

#if PET_P4_SYSTEM_WIFI
static bool wireless_owner_present(const uint8_t *owner) {
  if (!owner) return false;
  for (size_t i = 0; i < PET_P4_WIRELESS_OTA_OWNER_BYTES; i++) {
    if (owner[i]) return true;
  }
  return false;
}

static bool wireless_admitted(const pet_p4_wireless_ota_peer_t *peer) {
  return peer && peer->usb_verified && wireless_owner_present(peer->owner)
    && pet_p4_is_wifi6_hardware();
}

static void wireless_id_hex(const uint8_t *id, char out[33]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < PET_P4_WIRELESS_OTA_OWNER_BYTES; i++) {
    out[i * 2] = digits[id[i] >> 4];
    out[i * 2 + 1] = digits[id[i] & 15];
  }
  out[32] = '\0';
}

static bool wireless_transfer_matches(const uint8_t *owner, const uint8_t *id) {
  if (!g_active || !id || memcmp(owner, g_wireless_owner, sizeof(g_wireless_owner)) != 0) {
    return false;
  }
  char text[33];
  wireless_id_hex(id, text);
  return strcmp(g_transfer_id, text) == 0;
}

static const char *wireless_boot_locked(pet_p4_wireless_ota_boot_t *out) {
  const char *id = pet_p4_boot_identity();
  if (!g_initialized || !id || !id[0] || strlen(id) >= sizeof(out->boot_id)) {
    return "firmware boot identity unavailable";
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running || esp_partition_get_sha256(running, out->digest) != ESP_OK) {
    return "running firmware digest unavailable";
  }
  esp_ota_img_states_t state;
  out->valid = !g_pending_verify
    && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID;
  out->address = running->address;
  strcpy(out->boot_id, id);
  return NULL;
}

const char *pet_p4_wireless_ota_boot(const pet_p4_wireless_ota_peer_t *peer,
                                    pet_p4_wireless_ota_boot_t *out) {
  if (!out) return "invalid firmware status output";
  memset(out, 0, sizeof(*out));
  if (!wireless_admitted(peer)) return "wireless firmware controller not trusted";
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) {
    return "firmware OTA unavailable";
  }
  const char *error = wireless_boot_locked(out);
  xSemaphoreGive(g_ota_mutex);
  return error;
}

const char *pet_p4_wireless_ota_begin(const pet_p4_wireless_ota_peer_t *peer,
                                     const pet_p4_wireless_ota_offer_t *offer) {
  if (!wireless_admitted(peer)) return "wireless firmware controller not trusted";
  if (!offer || !wireless_owner_present(offer->transfer_id) ||
      !memchr(offer->prior.boot_id, '\0', sizeof(offer->prior.boot_id))) {
    return "invalid wireless firmware offer";
  }
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) {
    return "firmware OTA unavailable";
  }
  pet_p4_wireless_ota_boot_t boot = {0};
  const char *error = "firmware OTA busy";
  if (!g_active && !g_restart_at_ms) {
    error = wireless_boot_locked(&boot);
    if (!error && (!boot.valid || !offer->prior.valid || boot.address != offer->prior.address
        || strcmp(boot.boot_id, offer->prior.boot_id) != 0
        || memcmp(boot.digest, offer->prior.digest, sizeof(boot.digest)) != 0)) {
      error = "running firmware does not match validated boot";
    }
    if (!error) {
      char id[33], sha256[65];
      wireless_id_hex(offer->transfer_id, id);
      digest_hex(offer->image_sha256, sha256);
      error = begin_transfer(id, sha256, offer->image_bytes, true,
        offer->revision_min, offer->revision_max);
      if (!error) {
        memcpy(g_wireless_owner, peer->owner, sizeof(g_wireless_owner));
        memcpy(g_wireless_installed_digest, offer->installed_digest,
          sizeof(g_wireless_installed_digest));
      }
    }
  }
  xSemaphoreGive(g_ota_mutex);
  return error;
}

const char *pet_p4_wireless_ota_chunk(const pet_p4_wireless_ota_peer_t *peer,
                                    const uint8_t transfer_id[PET_P4_WIRELESS_OTA_OWNER_BYTES], uint32_t sequence,
                                    const uint8_t *data, size_t size) {
  if (!wireless_admitted(peer)) return "wireless firmware controller not trusted";
  if (!data || !size || size > PET_P4_WIRELESS_OTA_CHUNK_BYTES) {
    return "invalid wireless firmware chunk";
  }
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) {
    return "firmware OTA unavailable";
  }
  const char *error = "wireless firmware transfer owner mismatch";
  if (wireless_transfer_matches(peer->owner, transfer_id)) {
    error = "wireless firmware chunk sequence or size mismatch";
    if (sequence == g_next_sequence && size <= g_expected_size - g_received_size) {
      g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
      error = write_chunk(sequence, data, size);
    }
  }
  xSemaphoreGive(g_ota_mutex);
  return error;
}

const char *pet_p4_wireless_ota_commit(const pet_p4_wireless_ota_peer_t *peer,
                                     const uint8_t transfer_id[PET_P4_WIRELESS_OTA_OWNER_BYTES],
                                     pet_p4_wireless_ota_receipt_t *out) {
  if (!out) return "invalid firmware commit output";
  memset(out, 0, sizeof(*out));
  if (!wireless_admitted(peer)) return "wireless firmware controller not trusted";
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) {
    return "firmware OTA unavailable";
  }
  const char *error = "wireless firmware transfer owner mismatch";
  if (wireless_transfer_matches(peer->owner, transfer_id)) {
    uint32_t address = g_update_partition->address;
    error = commit_transfer(true, out->installed_digest);
    if (error) memset(out, 0, sizeof(*out));
    else out->address = address;
  }
  xSemaphoreGive(g_ota_mutex);
  return error;
}

void pet_p4_wireless_ota_close(const uint8_t owner[PET_P4_WIRELESS_OTA_OWNER_BYTES]) {
  if (!wireless_owner_present(owner) || !g_ota_mutex
      || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  if (g_active && memcmp(owner, g_wireless_owner, sizeof(g_wireless_owner)) == 0) {
    clear_transfer(true);
  }
  xSemaphoreGive(g_ota_mutex);
}
#endif

bool pet_p4_ota_handle_topic(
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (!topic) return false;
  bool handled = true;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return false;
  if (!g_initialized) {
    const char *transfer_id = json_string(payload, "transferId");
    if (strcmp(topic, "firmware/query") == 0) {
      send_status_locked(payload, send_line, ctx);
    } else if (strcmp(topic, "firmware/chunk") == 0) {
      send_chunk_ack(
        send_line,
        ctx,
        transfer_id,
        false,
        "firmware OTA is initializing",
        json_u64(payload, "seq", ULLONG_MAX),
        0,
        false
      );
    } else if (strcmp(topic, "firmware/begin") == 0
               || strcmp(topic, "firmware/commit") == 0
               || strcmp(topic, "firmware/abort") == 0) {
      send_ack(
        send_line,
        ctx,
        transfer_id,
        topic + strlen("firmware/"),
        false,
        "firmware OTA is initializing"
      );
    } else {
      handled = false;
    }
    xSemaphoreGive(g_ota_mutex);
    return handled;
  }
#if PET_P4_SYSTEM_WIFI
  if (g_active && wireless_owner_present(g_wireless_owner)) {
    const char *id = json_string(payload, "transferId");
    const char *error = "wireless firmware transfer owns OTA";
    if (strcmp(topic, "firmware/chunk") == 0) {
      send_chunk_ack(send_line, ctx, id, false, error, json_u64(payload, "seq", ULLONG_MAX), 0, false);
      xSemaphoreGive(g_ota_mutex);
      return true;
    }
    if (strcmp(topic, "firmware/begin") == 0 || strcmp(topic, "firmware/commit") == 0
        || strcmp(topic, "firmware/abort") == 0) {
      send_ack(send_line, ctx, id, topic + strlen("firmware/"), false, error);
      xSemaphoreGive(g_ota_mutex);
      return true;
    }
  }
#endif
  if (strcmp(topic, "firmware/begin") == 0) {
    handle_begin(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/chunk") == 0) {
    handle_chunk(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/commit") == 0) {
    handle_commit(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/abort") == 0) {
    handle_abort(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/query") == 0) {
    send_status_locked(payload, send_line, ctx);
  } else {
    handled = false;
  }
  xSemaphoreGive(g_ota_mutex);
  return handled;
}

void pet_p4_ota_send_status(pet_p4_send_line_fn send_line, void *ctx) {
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  send_status_locked(NULL, send_line, ctx);
  xSemaphoreGive(g_ota_mutex);
}

void pet_p4_ota_runtime_ready(unsigned long long now_ms) {
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  if (!g_runtime_last_healthy_ms
      || now_ms - g_runtime_last_healthy_ms > PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS) {
    g_runtime_first_healthy_ms = now_ms;
    g_runtime_healthy_samples = 0;
  }
  g_runtime_last_healthy_ms = now_ms;
  g_runtime_healthy_samples += 1;
  xSemaphoreGive(g_ota_mutex);
}

void pet_p4_ota_runtime_failed(unsigned long long now_ms) {
  (void) now_ms;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  g_runtime_ready = false;
  g_runtime_healthy_samples = 0;
  g_runtime_first_healthy_ms = 0;
  g_runtime_last_healthy_ms = 0;
  xSemaphoreGive(g_ota_mutex);
}

void pet_p4_ota_process(unsigned long long now_ms) {
  bool rollback = false;
  bool restart = false;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  if (g_active && g_last_activity_ms
      && now_ms >= g_last_activity_ms
      && now_ms - g_last_activity_ms >= PET_P4_OTA_TRANSFER_IDLE_TIMEOUT_MS) {
    ESP_LOGW(TAG, "aborting idle OTA transfer=%s after %llu ms", g_transfer_id,
             now_ms - g_last_activity_ms);
    clear_transfer(true);
  }
  g_runtime_ready = g_runtime_healthy_samples >= PET_P4_OTA_VALIDATE_MIN_SAMPLES
    && g_runtime_first_healthy_ms
    && now_ms - g_runtime_first_healthy_ms >= PET_P4_OTA_VALIDATE_STABLE_MS
    && now_ms - g_runtime_last_healthy_ms <= PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS;
  if (g_pending_verify && g_runtime_ready) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
#if PET_P4_OTA_METADATA_CACHED
    observe_image_state(err,ESP_OTA_IMG_VALID);
#endif
    if (err == ESP_OK) {
      g_pending_verify = false;
#if PET_P4_BOOT_TRACE_ENABLE
      (void)pet_boot_trace_mark(PET_BOOT_IMAGE_VALID);
#endif
      ESP_LOGI(TAG, "OTA image marked valid after sustained runtime validation");
    } else {
      ESP_LOGE(TAG, "failed to mark OTA image valid: %s", esp_err_to_name(err));
    }
  }
  if (g_pending_verify
      && now_ms - g_initialized_ms >= PET_P4_OTA_FAILED_BOOT_RESTART_MS) {
    rollback = true;
  }
  if (g_restart_at_ms && now_ms >= g_restart_at_ms) {
    restart = true;
  }
  xSemaphoreGive(g_ota_mutex);
  if (rollback) {
    ESP_LOGE(TAG, "runtime self-test did not stay healthy; rolling back OTA image");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  if (restart) {
#if PET_P4_SYSTEM_WIFI
    if (!pet_device_wifi_prepare_reboot()) return;
#endif
    ESP_LOGI(TAG, "restarting into verified OTA image");
    esp_restart();
  }
}

bool pet_p4_ota_active(void) {
  bool active = false;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return false;
  active = g_active || g_pending_verify || g_restart_at_ms != 0;
  xSemaphoreGive(g_ota_mutex);
  return active;
}

bool pet_p4_ota_transfer_active(void) {
  bool active = false;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return false;
  active = g_active;
  xSemaphoreGive(g_ota_mutex);
  return active;
}
