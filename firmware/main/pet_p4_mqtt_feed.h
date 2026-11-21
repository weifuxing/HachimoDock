#pragma once

#if PET_P4_SYSTEM_WIFI
#include "pet_p4_private_feed.h"

typedef bool (*pet_mqtt_feed_current_fn)(void *ctx);
typedef void (*pet_mqtt_feed_value_fn)(const char *json, size_t size, void *ctx);

/* Blocks only the low-priority feed worker while this source is active. */
bool pet_mqtt_feed_run(const pet_wifi_endpoint_t *endpoint, const pet_private_feed_t *binding,
                       const pet_private_secret_t *secret, char *workspace,
                       pet_mqtt_feed_current_fn current, pet_mqtt_feed_value_fn accept, void *ctx);
#endif
