#include "pet_p4_private_feed.h"

#if PET_P4_SYSTEM_WIFI
#include <stdatomic.h>
#include <stdio.h>
#include "bootloader_random.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CREDENTIAL_SLOTS 8
static atomic_uint revision;
static atomic_flag writer = ATOMIC_FLAG_INIT;
static atomic_int local_result;
static struct {
  pet_private_feed_t feed;
  pet_private_secret_t secret;
  bool forget;
} local_request;
static uint8_t boot_nonce[12];
static uint32_t nonce_index;
static bool crypto_ready;
static const char purpose[] = "HachimiDock/PetUI/credentials/v1";

typedef struct {
  uint8_t id[32];
  uint8_t iv[12];
  uint8_t tag[16];
  uint8_t ciphertext[sizeof(pet_private_secret_t)];
} credential_t;

static bool credential_key_block(esp_efuse_block_t *block) {
  return esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, block) &&
    esp_efuse_get_key_dis_read(*block) && esp_efuse_get_key_dis_write(*block) &&
    esp_efuse_get_keypurpose_dis_write(*block);
}

void pet_private_storage_init(void) {
  esp_efuse_block_t block;
  if (!credential_key_block(&block)) return;
  /* At entry to app_main there is no joystick/ADC worker. Sample with the
   * main entropy source before starting any peripheral that may use it. */
  bootloader_random_enable();
  esp_fill_random(boot_nonce, sizeof(boot_nonce));
  bootloader_random_disable();
  crypto_ready = true;
}

bool pet_private_storage_ready(void) {
  return crypto_ready;
}

uint32_t pet_private_storage_epoch(void) {
  return atomic_load(&revision);
}

static bool mqtt_endpoint(const char *url, pet_wifi_endpoint_t *endpoint, uint16_t *port) {
  if (strncmp(url, "mqtts://", 8)) return false;
  const char *authority = url + 8;
  const char *colon = strchr(authority, ':');
  const size_t length = colon ? (size_t)(colon - authority) : strlen(authority);
  if (!length || length >= sizeof(endpoint->host) || strpbrk(authority, "/?#@")) return false;
  unsigned number = 8883;
  if (colon) {
    number = 0;
    const char *digits = colon + 1;
    if (!*digits || strlen(digits) > 5) return false;
    for (; *digits; ++digits) {
      if (*digits < '0' || *digits > '9') return false;
      number = number * 10 + (*digits - '0');
    }
    if (!number || number > 65535) return false;
  }
  char https[sizeof(endpoint->host) + 8];
  snprintf(https, sizeof(https), "https://%.*s", (int)length, authority);
  if (!pet_wifi_endpoint_parse(https, endpoint)) return false;
  *port = number;
  return true;
}

static bool topic_valid(const char *topic) {
  if (!topic || !*topic || strlen(topic) > PET_PRIVATE_TOPIC_MAX ||
      topic[0] == '$' || strpbrk(topic, "+#")) return false;
  for (const unsigned char *p = (const unsigned char *)topic; *p; ++p)
    if (*p < 33 || *p > 126) return false;
  return true;
}

static bool feed_binding(pet_private_feed_t *out, pet_wifi_endpoint_t *endpoint, const char *component,
                 const char *url, pet_private_kind_t kind, const char *topic) {
  if (!component || !*component || strlen(component) > 47 || !url) return false;
  for (const char *p = component; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.", *p))) return false;
  *out = (pet_private_feed_t){.kind = kind, .port = 443};
  if (kind == PET_PRIVATE_BEARER) {
    /* Credentials belong in Authorization. Disallow query strings in this
     * first profile so an exported declaration cannot hide them in a URL. */
    if (topic || strchr(url, '?') || !pet_wifi_endpoint_parse(url, endpoint) ||
        !strncmp(endpoint->path, "//", 2)) return false;
  } else if (kind == PET_PRIVATE_MQTT || kind == PET_PUBLIC_MQTT) {
    if (!topic_valid(topic) || !mqtt_endpoint(url, endpoint, &out->port)) return false;
    strcpy(out->topic, topic);
  } else {
    return false;
  }
  char key[48 + sizeof(endpoint->host) + sizeof(endpoint->path) + sizeof(out->topic) + 24];
  const int length = snprintf(key, sizeof(key), "%u\n%s\n%s\n%u\n%s\n%s",
    (unsigned)kind, component, endpoint->host, out->port, endpoint->path, out->topic);
  return length > 0 && (size_t)length < sizeof(key) &&
    !mbedtls_sha256((const uint8_t *)key, (size_t)length, out->id, 0);
}

bool pet_private_feed_parse(pet_private_feed_t *out, pet_wifi_endpoint_t *endpoint,
                            const char *component, const char *url, const cJSON *connection) {
  if (!cJSON_IsObject(connection)) return false;
  const cJSON *kind = cJSON_GetObjectItemCaseSensitive(connection, "type");
  const cJSON *topic = cJSON_GetObjectItemCaseSensitive(connection, "topic");
  if (!cJSON_IsString(kind)) return false;
  const int fields = cJSON_GetArraySize(connection);
  if (fields == 1 && !strcmp(kind->valuestring, "bearer"))
    return feed_binding(out, endpoint, component, url, PET_PRIVATE_BEARER, NULL);
  if (fields == 2 && !strcmp(kind->valuestring, "mqtt-public") && cJSON_IsString(topic))
    return feed_binding(out, endpoint, component, url, PET_PUBLIC_MQTT, topic->valuestring);
  if (fields == 2 && !strcmp(kind->valuestring, "mqtt") && cJSON_IsString(topic))
    return feed_binding(out, endpoint, component, url, PET_PRIVATE_MQTT, topic->valuestring);
  return false;
}

static bool secret_valid(const pet_private_feed_t *feed, const pet_private_secret_t *secret) {
  if (feed->kind == PET_PRIVATE_BEARER) {
    if (secret->user[0] || !secret->token[0]) return false;
    for (const char *p = secret->token; *p; ++p)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || strchr("-._~+/=", *p))) return false;
  } else if (secret->token[0] && !secret->user[0]) return false;
  return true;
}

static bool setup_key(mbedtls_gcm_context *gcm) {
  esp_efuse_block_t block;
  uint8_t key[32];
  const bool ok = credential_key_block(&block) &&
    esp_hmac_calculate((hmac_key_id_t)(block - EFUSE_BLK_KEY0), purpose, sizeof(purpose), key) == ESP_OK &&
    mbedtls_gcm_setkey(gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0;
  mbedtls_platform_zeroize(key, sizeof(key));
  return ok;
}

/* Each slot is an atomic NVS blob. Only the LAN worker writes; the network
 * reader gets a whole previous or new revision, never a partial token. */
static pet_private_result_t credential(const pet_private_feed_t *feed, uint8_t op,
                                       pet_private_secret_t *secret) {
  if (!pet_private_storage_ready()) return PET_PRIVATE_NO_STORAGE;
  nvs_handle_t nvs;
  const esp_err_t open = nvs_open("pet_credentials", op == 1 ? NVS_READWRITE : NVS_READONLY, &nvs);
  if (open == ESP_ERR_NVS_NOT_FOUND) return PET_PRIVATE_ABSENT;
  if (open != ESP_OK) return PET_PRIVATE_STORAGE_ERROR;
  credential_t record;
  int found = -1, free_slot = -1;
  char slot[] = "feed0";
  pet_private_result_t result = PET_PRIVATE_STORAGE_ERROR;
  for (int i = 0; i < CREDENTIAL_SLOTS; ++i) {
    slot[4] = '0' + i;
    size_t length = sizeof(record);
    esp_err_t status = nvs_get_blob(nvs, slot, &record, &length);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
      if (free_slot < 0) free_slot = i;
    } else if (status != ESP_OK || length != sizeof(record)) {
      goto done;
    } else if (!memcmp(record.id, feed->id, sizeof(record.id))) {
      found = i;
      break;
    }
  }
  if (op == 2 && found < 0) { result = PET_PRIVATE_ABSENT; goto done; }
  if (op != 1 && op != 2 && found < 0) { result = PET_PRIVATE_ABSENT; goto done; }
  if (op == 2) {
    nvs_close(nvs);
    if (nvs_open("pet_credentials", NVS_READWRITE, &nvs) != ESP_OK) return PET_PRIVATE_STORAGE_ERROR;
    if (nvs_erase_key(nvs, slot) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
      atomic_fetch_add(&revision, 1);
      result = PET_PRIVATE_ABSENT;
    }
    goto done;
  }
  if (op == 1 && found < 0 && free_slot < 0) goto done;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  if (setup_key(&gcm)) {
    if (op == 1 && nonce_index != UINT32_MAX) {
      slot[4] = '0' + (found < 0 ? free_slot : found);
      memcpy(record.id, feed->id, sizeof(record.id));
      memcpy(record.iv, boot_nonce, sizeof(record.iv));
      const uint32_t index = nonce_index++;
      for (unsigned i = 0; i < 4; ++i)
        record.iv[8 + i] ^= (uint8_t)(index >> (24 - 8 * i));
      if (!mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(record.ciphertext),
          record.iv, sizeof(record.iv), record.id, sizeof(record.id), (const uint8_t *)secret,
          record.ciphertext, sizeof(record.tag), record.tag) &&
          nvs_set_blob(nvs, slot, &record, sizeof(record)) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
        atomic_fetch_add(&revision, 1);
        result = PET_PRIVATE_SET;
      }
    } else if (op == 0 && !mbedtls_gcm_auth_decrypt(&gcm, sizeof(record.ciphertext), record.iv, sizeof(record.iv),
        record.id, sizeof(record.id), record.tag, sizeof(record.tag), record.ciphertext, (uint8_t *)secret) &&
        !secret->user[PET_PRIVATE_USER_MAX] && !secret->token[PET_PRIVATE_TOKEN_MAX] &&
        secret_valid(feed, secret)) {
      result = PET_PRIVATE_SET;
    }
  }
  mbedtls_gcm_free(&gcm);
done:
  nvs_close(nvs);
  if (op == 0 && result != PET_PRIVATE_SET) mbedtls_platform_zeroize(secret, sizeof(*secret));
  mbedtls_platform_zeroize(&record, sizeof(record));
  return result;
}

static pet_private_result_t write_credential(const pet_private_feed_t *feed, uint8_t op,
                                              pet_private_secret_t *secret) {
  if (atomic_flag_test_and_set(&writer)) return PET_PRIVATE_BUSY;
  pet_private_result_t result = credential(feed, op, secret);
  atomic_flag_clear(&writer);
  return result;
}

static void credential_worker(void *unused) {
  (void)unused;
  pet_private_result_t result = write_credential(&local_request.feed, local_request.forget ? 2 : 1,
                                                  &local_request.secret);
  mbedtls_platform_zeroize(&local_request, sizeof(local_request));
  atomic_store(&local_result, result == PET_PRIVATE_BUSY ? PET_PRIVATE_STORAGE_ERROR : result);
  vTaskDelete(NULL);
}

bool pet_private_secret_schedule(const pet_private_feed_t *feed, const pet_private_secret_t *secret,
                                 bool forget) {
  if (!pet_private_storage_ready() || (!forget && !secret_valid(feed, secret)) ||
      atomic_load(&local_result) == PET_PRIVATE_BUSY) return false;
  local_request.feed = *feed;
  local_request.secret = *secret;
  local_request.forget = forget;
  atomic_store(&local_result, PET_PRIVATE_BUSY);
  if (xTaskCreate(credential_worker, "pet_feed_cred", 4096, NULL, 2, NULL) == pdPASS) return true;
  mbedtls_platform_zeroize(&local_request, sizeof(local_request));
  atomic_store(&local_result, PET_PRIVATE_STORAGE_ERROR);
  return false;
}

pet_private_result_t pet_private_local_result(void) {
  return atomic_load(&local_result);
}

pet_private_result_t pet_private_secret_read(const pet_private_feed_t *feed,
                                              pet_private_secret_t *out) {
  return credential(feed, 0, out);
}

static bool take_text(const uint8_t **at, size_t *remaining, char *out, size_t capacity, size_t n) {
  if (n >= capacity || n > *remaining) return false;
  for (size_t i = 0; i < n; ++i) if ((*at)[i] < 32 || (*at)[i] > 126) return false;
  memcpy(out, *at, n);
  out[n] = 0;
  *at += n;
  *remaining -= n;
  return true;
}

pet_private_result_t pet_private_credentials_request(const uint8_t *at, size_t remaining) {
  if (remaining < 9 || at[0] > 2) return PET_PRIVATE_REFUSED;
  const uint8_t command = at[0];
  const pet_private_kind_t kind = at[1];
  if (!pet_feed_requires_account(kind)) return PET_PRIVATE_REFUSED;
  const unsigned id = at[2], url = at[3] * 256u + at[4], topic = at[5];
  const unsigned user = at[6], token = at[7] * 256u + at[8];
  if ((command != 1 && (user || token)) || (kind == PET_PRIVATE_BEARER && topic))
    return PET_PRIVATE_REFUSED;
  at += 9;
  remaining -= 9;
  char component[48], address[392], filter[PET_PRIVATE_TOPIC_MAX + 1];
  pet_private_secret_t secret = {0};
  pet_wifi_endpoint_t endpoint;
  pet_private_feed_t feed;
  pet_private_result_t result = PET_PRIVATE_REFUSED;
  if (take_text(&at, &remaining, component, sizeof(component), id) &&
      take_text(&at, &remaining, address, sizeof(address), url) &&
      take_text(&at, &remaining, filter, sizeof(filter), topic) &&
      take_text(&at, &remaining, secret.user, sizeof(secret.user), user) &&
      take_text(&at, &remaining, secret.token, sizeof(secret.token), token) && !remaining &&
      feed_binding(&feed, &endpoint, component, address, kind, topic ? filter : NULL) &&
      (command != 1 || secret_valid(&feed, &secret))) {
    result = command ? write_credential(&feed, command, &secret) : credential(&feed, 0, &secret);
  }
  mbedtls_platform_zeroize(&secret, sizeof(secret));
  return result;
}
#endif
