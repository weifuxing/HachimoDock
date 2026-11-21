#pragma once

#if PET_P4_SYSTEM_WIFI
#include "pet_p4_wifi_endpoint.h"
#include "cJSON.h"

#define PET_PRIVATE_FEED_CAPABILITY "p4-private-feed-v1"
#define PET_PRIVATE_FEED_REQUEST_MAX 960
#define PET_PRIVATE_TOPIC_MAX 127
#define PET_PRIVATE_USER_MAX 64
#define PET_PRIVATE_TOKEN_MAX 256

typedef enum {
  PET_PRIVATE_NONE,
  PET_PRIVATE_BEARER,
  PET_PRIVATE_MQTT,
  PET_PUBLIC_MQTT,
} pet_private_kind_t;

static inline bool pet_feed_requires_account(pet_private_kind_t kind) {
  return kind == PET_PRIVATE_BEARER || kind == PET_PRIVATE_MQTT;
}

typedef struct {
  pet_private_kind_t kind;
  uint16_t port;
  char topic[PET_PRIVATE_TOPIC_MAX + 1];
  uint8_t id[32];
} pet_private_feed_t;

typedef struct {
  char user[PET_PRIVATE_USER_MAX + 1];
  char token[PET_PRIVATE_TOKEN_MAX + 1];
} pet_private_secret_t;

typedef enum {
  PET_PRIVATE_REFUSED,
  PET_PRIVATE_SET,
  PET_PRIVATE_ABSENT,
  PET_PRIVATE_NO_STORAGE,
  PET_PRIVATE_STORAGE_ERROR,
  PET_PRIVATE_BUSY,
} pet_private_result_t;

bool pet_private_feed_parse(pet_private_feed_t *out, pet_wifi_endpoint_t *endpoint,
                            const char *component, const char *url, const cJSON *connection);
void pet_private_storage_init(void);
bool pet_private_storage_ready(void);
uint32_t pet_private_storage_epoch(void);
bool pet_private_secret_schedule(const pet_private_feed_t *binding,
                                 const pet_private_secret_t *secret, bool forget);
pet_private_result_t pet_private_local_result(void);
pet_private_result_t pet_private_secret_read(const pet_private_feed_t *binding,
                                              pet_private_secret_t *out);
/* Called only on the authenticated LAN worker. The request contains a boot
 * prefix that the caller must verify before handing off the remainder. */
pet_private_result_t pet_private_credentials_request(const uint8_t *request, size_t size);
#endif
