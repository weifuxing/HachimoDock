#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"

#define PET_WIFI_FEED_HTTP_CAPABILITY "p4-wifi-feed-http-v1"

typedef struct {
  uint8_t accept;
  bool revalidate;
  bool persist;
} pet_wifi_feed_http_t;

/* The package cannot set arbitrary headers or interpolate device state. */
static const char *const pet_wifi_feed_media_types[] = {
  NULL, "application/json", "application/geo+json", "application/hal+json",
  "application/vnd.api+json",
};

static inline bool pet_wifi_feed_http_parse(const cJSON *config, pet_wifi_feed_http_t *out) {
  *out = (pet_wifi_feed_http_t){0};
  if (!config) return true;
  if (!cJSON_IsObject(config) || !config->child) return false;
  const cJSON *field;
  cJSON_ArrayForEach(field, config) {
    if (!strcmp(field->string, "accept")) {
      const char *value = cJSON_GetStringValue(field);
      if (out->accept || !value) return false;
      for (unsigned i = 1; i < sizeof(pet_wifi_feed_media_types) / sizeof(pet_wifi_feed_media_types[0]); i++) {
        if (!strcmp(value, pet_wifi_feed_media_types[i])) out->accept = i;
      }
      if (!out->accept) return false;
    } else if (!strcmp(field->string, "revalidate")) {
      if (out->revalidate || !cJSON_IsTrue(field)) return false;
      out->revalidate = true;
    } else if (!strcmp(field->string, "cache")) {
      const char *value = cJSON_GetStringValue(field);
      if (out->persist || !value || strcmp(value, "last-good")) return false;
      out->persist = true;
    } else return false;
  }
  return true;
}
