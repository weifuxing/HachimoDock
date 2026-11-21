#pragma once

#if PET_P4_SYSTEM_WIFI
#include "pet_p4_wifi_endpoint.h"
#include "pet_p4_wifi_feed_http.h"
#include "pet_p4_wifi_data_sources.h"
#include "pet_p4_private_feed.h"

#define PET_WIFI_DATA_CAPABILITY "p4-wifi-data-v1"
#define PET_WIFI_DATA_FEED_CAPABILITY "p4-wifi-feed-v1"
#define PET_WIFI_DATA_FEED_MAP_CAPABILITY "p4-wifi-feed-map-v1"
#define PET_WIFI_NEWS_AGE_LIMIT_MS 3600000ULL

typedef struct {
  pet_wifi_endpoint_t endpoint;
  char source[48]; /* Internal source key, scoped to component, endpoint and mapping. */
  pet_wifi_feed_map_t response_map;
  pet_wifi_feed_http_t http;
  pet_private_feed_t private;
} pet_wifi_data_feed_t;

/* Parse a package-declared URL. On success, use source for widget data reads. */
bool pet_wifi_data_feed_init(pet_wifi_data_feed_t *out, const char *component_id, const char *url,
                             const cJSON *response_map, const cJSON *http,
                             const cJSON *connection);

/* Called on the miniapp host-process lane. Only the visible source is polled. */
void pet_wifi_data_observe(const char *source);
void pet_wifi_data_observe_feed(const pet_wifi_data_feed_t *feed);
void pet_wifi_data_refresh(const char *source);
bool pet_wifi_data_authorization_needed(const pet_private_feed_t *binding);
bool pet_wifi_data_retrying(const char *source);

/* Copy validated public settings from the existing PC snapshot into a
 * one-element mailbox. The worker owns persistent settings and TLS. */
void pet_wifi_data_set_watchlist(const pet_wifi_watchlist_t *list, uint64_t revision);
#endif
