#pragma once

#if PET_P4_SYSTEM_WIFI
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_p4_widget_data.h"

#define PET_WIFI_DATA_STOCKS "stocks.watchlist"
#define PET_WIFI_DATA_NEWS "news.hn"
#define PET_WIFI_DATA_FEED "wifi.feed"
#define PET_WIFI_DATA_FEED_PREFIX PET_WIFI_DATA_FEED "."
#define PET_WIFI_DATA_FEED_TTL_MS 120000
#define PET_WIFI_DATA_BODY_MAX 16384
#define PET_WIFI_DATA_URL_MAX 384
#define PET_WIFI_FEED_PATH_MAX 63

typedef enum {
  PET_WIFI_FEED_ROWS, PET_WIFI_FEED_ID, PET_WIFI_FEED_LABEL, PET_WIFI_FEED_VALUE,
  PET_WIFI_FEED_DETAIL, PET_WIFI_FEED_META, PET_WIFI_FEED_TONE, PET_WIFI_FEED_FIELD_COUNT,
} pet_wifi_feed_field_t;

typedef struct {
  char paths[PET_WIFI_FEED_FIELD_COUNT][PET_WIFI_FEED_PATH_MAX + 1];
} pet_wifi_feed_map_t;

/* A missing declaration selects the original schema=1 response format. */
bool pet_wifi_feed_map_parse(const cJSON *declaration, pet_wifi_feed_map_t *out);

typedef struct {
  unsigned count;
  char symbols[PET_P4_DATA_MAX_ROWS][24];
} pet_wifi_watchlist_t;

typedef enum {
  PET_WIFI_GET_TENCENT_QUOTES,
  PET_WIFI_GET_HN_TOP,
  PET_WIFI_GET_HN_ITEM,
} pet_wifi_data_endpoint_t;

/* Endpoint identifiers and typed arguments are the entire network allowlist. */
bool pet_wifi_data_url(char out[PET_WIFI_DATA_URL_MAX], pet_wifi_data_endpoint_t endpoint,
                       const pet_wifi_watchlist_t *list, uint32_t item);
bool pet_wifi_watchlist_equal(const pet_wifi_watchlist_t *a, const pet_wifi_watchlist_t *b);
bool pet_wifi_watchlist_parse(const cJSON *symbols, pet_wifi_watchlist_t *out);
cJSON *pet_wifi_stock_snapshot(char *body, const pet_wifi_watchlist_t *list, uint64_t utc_ms);
bool pet_wifi_news_ids(const char *body, size_t length, uint32_t ids[8], unsigned *count);
bool pet_wifi_news_add(cJSON *snapshot, const char *body, size_t length, uint32_t id, uint64_t utc_ms);
cJSON *pet_wifi_data_snapshot(const char *source, const char *status, const char *message,
                            uint32_t ttl_ms, uint64_t utc_ms);
/* Parse the original schema=1 envelope or project a validated response map.
 * The widget data validator still checks row values and unique IDs. */
cJSON *pet_wifi_feed_snapshot(const char *source, const char *body, size_t length,
                             const pet_wifi_feed_map_t *map);
#endif
