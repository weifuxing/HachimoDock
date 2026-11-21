#include "pet_p4_wifi_data_cache.h"

#if PET_P4_SYSTEM_WIFI
#include <string.h>

#include "esp_heap_caps.h"
#include "nvs.h"
#include "pet_p4_json_source.h"

#define CACHE_NAMESPACE "pet_wifi_cache"
#define STOCK_CACHE_LIMIT 6144
#define NEWS_CACHE_LIMIT 2048
#define FEED_CACHE_LIMIT 4096

static bool feed_key(const char *source) {
  const size_t prefix = sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1;
  return !strncmp(source, PET_WIFI_DATA_FEED_PREFIX, prefix) && strlen(source + prefix) == 32 &&
    strspn(source + prefix, "0123456789abcdef") == 32;
}

bool pet_wifi_data_cache_good_feed(const cJSON *snapshot) {
  const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(snapshot, "status"));
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(snapshot, "rows");
  return status && cJSON_IsArray(rows) && cJSON_GetArraySize(rows) <= PET_P4_DATA_MAX_ROWS &&
    (!strcmp(status, "ok") || (!strcmp(status, "empty") && !rows->child));
}

static const char *cache_key(const char *source, size_t *limit) {
  if (!strcmp(source, PET_WIFI_DATA_STOCKS)) {
    *limit = STOCK_CACHE_LIMIT;
    return "stocks_v1";
  }
  if (!strcmp(source, PET_WIFI_DATA_NEWS)) {
    *limit = NEWS_CACHE_LIMIT;
    return "news_v1";
  }
  if (feed_key(source)) {
    *limit = FEED_CACHE_LIMIT;
    return "feed_v1";
  }
  return NULL;
}

static bool matches(const cJSON *snapshot, const char *source, const pet_wifi_watchlist_t *list) {
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(snapshot, "source");
  const cJSON *status = cJSON_GetObjectItemCaseSensitive(snapshot, "status");
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(snapshot, "rows");
  if (!cJSON_IsString(name) || strcmp(name->valuestring, source) ||
      cJSON_HasObjectItem(snapshot, "clearOnExpire")) return false;
  if (feed_key(source)) return pet_wifi_data_cache_good_feed(snapshot);
  if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") ||
      !cJSON_IsArray(rows) || !rows->child) return false;
  if (strcmp(source, PET_WIFI_DATA_STOCKS)) return cJSON_GetArraySize(rows) <= PET_P4_DATA_PAGE_ROWS;
  if (!list || cJSON_GetArraySize(rows) != (int)list->count) return false;
  unsigned i = 0;
  const cJSON *row;
  cJSON_ArrayForEach(row, rows) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
    if (!cJSON_IsString(id) || strcmp(id->valuestring, list->symbols[i++])) return false;
  }
  return true;
}

bool pet_wifi_data_cache_restore(const char *source, const pet_wifi_watchlist_t *list,
                                uint64_t now_ms, uint64_t revision) {
  size_t limit;
  const char *key = cache_key(source, &limit);
  nvs_handle_t handle;
  if (!key || nvs_open(CACHE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t length = 0;
  char *raw = NULL;
  size_t capacity = 0;
  esp_err_t result = nvs_get_blob(handle, key, NULL, &length);
  if (result == ESP_OK && length > 1 && length <= limit) {
    capacity = length;
    raw = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw) result = nvs_get_blob(handle, key, raw, &length);
  }
  nvs_close(handle);
  if (!raw) return false;
  cJSON *snapshot = NULL;
  if (result == ESP_OK && length > 1 && length <= capacity && raw[length - 1] == 0 &&
      pet_json_source_text_limit_bytes(raw, length - 1, 3))
    snapshot = cJSON_ParseWithLengthOpts(raw, length, NULL, true);
  heap_caps_free(raw);
  const bool ok = snapshot && matches(snapshot, source, list) &&
    pet_p4_widget_data_restore_local(snapshot, now_ms, revision);
  cJSON_Delete(snapshot);
  return ok;
}

bool pet_wifi_data_cache_save(const char *source, const pet_wifi_watchlist_t *list,
                             const cJSON *snapshot) {
  size_t limit;
  const char *key = cache_key(source, &limit);
  if (!key || !matches(snapshot, source, list)) return false;
  char *raw = heap_caps_malloc(limit, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) return false;
  bool ok = false;
  nvs_handle_t handle;
  if (cJSON_PrintPreallocated((cJSON *)snapshot, raw, (int)limit, false) &&
      nvs_open(CACHE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
    ok = nvs_set_blob(handle, key, raw, strlen(raw) + 1) == ESP_OK &&
      nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
  }
  heap_caps_free(raw);
  return ok;
}
#endif
