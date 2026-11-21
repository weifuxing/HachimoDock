#pragma once

#include "pet_p4_wifi_data_sources.h"

#if PET_P4_SYSTEM_WIFI
/* Wi-Fi data worker only. The ordered list binds a stock record to its watchlist. */
bool pet_wifi_data_cache_restore(const char *source, const pet_wifi_watchlist_t *list,
                                uint64_t now_ms, uint64_t revision);
bool pet_wifi_data_cache_good_feed(const cJSON *snapshot);
bool pet_wifi_data_cache_save(const char *source, const pet_wifi_watchlist_t *list,
                             const cJSON *snapshot);
#endif
