#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "esp_netif_sntp.h"
#include "nvs.h"
#include "pet_p4_wifi_clock.h"
#include "pet_p4_wifi_data.h"
#include "pet_p4_wifi_data_cache.h"

static int64_t time_ms = 500;
static int32_t offset_saved = 480, timezone_saved = 345;
static bool fail_write;
static struct timeval wall_clock;
static void (*sync_callback)(struct timeval *);
static struct record { char bytes[6144]; size_t length; } stocks, news;

int64_t esp_timer_get_time(void) { return time_ms * 1000; }
int fixture_gettimeofday(struct timeval *value, void *timezone) {
  (void)timezone;
  *value = wall_clock;
  return 0;
}
void simulate_clock_reset(void);
int esp_netif_sntp_init(const esp_sntp_config_t *config) {
  sync_callback = config->sync_cb;
  return ESP_OK;
}
void esp_netif_sntp_deinit(void) { sync_callback = NULL; }
int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
  (void)mode;
  *handle = !strcmp(name, "pet_wifi_clock") ? 1 : 2;
  return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
int nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
int nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *value) {
  assert(handle == 1);
  *value = !strcmp(key, "timezone") ? timezone_saved : offset_saved;
  return ESP_OK;
}
int nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value) {
  assert(handle == 1);
  if (fail_write) return -1;
  *(!strcmp(key, "timezone") ? &timezone_saved : &offset_saved) = value;
  return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *length) {
  assert(handle == 2);
  const struct record *record = !strcmp(key, "stocks_v1") ? &stocks : &news;
  if (!record->length || (bytes && *length < record->length)) return -1;
  *length = record->length;
  if (bytes) memcpy(bytes, record->bytes, *length);
  return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t length) {
  assert(handle == 2);
  if (fail_write) return -1;
  struct record *record = !strcmp(key, "stocks_v1") ? &stocks : &news;
  assert(length <= sizeof(record->bytes));
  memcpy(record->bytes, bytes, length);
  record->length = length;
  return ESP_OK;
}
void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
void heap_caps_free(void *pointer) { free(pointer); }
void pet_wifi_data_set_watchlist(const pet_wifi_watchlist_t *list, uint64_t revision) {
  (void)list;
  (void)revision;
}

static void test_clock(void) {
  uint64_t utc, age;
  int32_t offset;
  pet_wifi_clock_load_offset();
  assert(pet_wifi_clock_timezone() == 345 && pet_wifi_clock_auto_offset() == 480);
  assert(!pet_wifi_clock_read(time_ms, &utc, &offset, &age));
  pet_wifi_clock_note_offset(-300);
  pet_wifi_clock_service(true);
  assert(sync_callback);
  struct timeval synced = {.tv_sec = 1794384000, .tv_usec = 250000};
  sync_callback(&synced);
  pet_wifi_clock_service(false);
  assert(!sync_callback);
  assert(pet_wifi_clock_read(time_ms + 1000, &utc, &offset, &age));
  assert(utc == 1794384001250ULL && age == 1000 && offset == 345);
  assert(!pet_wifi_clock_set_timezone(842));
  assert(pet_wifi_clock_set_timezone(PET_WIFI_TZ_AUTO));
  assert(pet_wifi_clock_read(time_ms + 2000, &utc, &offset, &age) && offset == -300);
  fail_write = true;
  pet_wifi_clock_service(false);
  assert(timezone_saved == 345);
  fail_write = false;
  time_ms += 30000;
  pet_wifi_clock_service(false);
  assert(timezone_saved == PET_WIFI_TZ_AUTO && offset_saved == -300);

  simulate_clock_reset();
  time_ms = 20;
  pet_wifi_clock_load_offset();
  assert(!pet_wifi_clock_read(time_ms, &utc, &offset, &age));
  wall_clock = (struct timeval){.tv_sec = synced.tv_sec + 60, .tv_usec = 500000};
  pet_wifi_clock_load_offset();
  assert(pet_wifi_clock_read(time_ms + 1000, &utc, &offset, &age));
  assert(utc == 1794384061500ULL && age == 61500 && offset == -300);
}

static void test_cache(void) {
  cJSON *stock = cJSON_Parse(
    "{\"schema\":1,\"source\":\"stocks.watchlist\",\"ttlMs\":30000,\"status\":\"ok\",\"message\":\"\","
    "\"rows\":[{\"id\":\"usAAPL\",\"label\":\"AAPL\",\"value\":\"5\",\"detail\":\"\",\"meta\":\"\",\"tone\":0}]}");
  const pet_wifi_watchlist_t list = {.count = 1, .symbols = {"usAAPL"}};
  const pet_wifi_watchlist_t wrong = {.count = 1, .symbols = {"usMSFT"}};
  assert(stock && pet_wifi_data_cache_save(PET_WIFI_DATA_STOCKS, &list, stock));
  fail_write = true;
  assert(!pet_wifi_data_cache_save(PET_WIFI_DATA_STOCKS, &list, stock));
  fail_write = false;
  assert(!pet_wifi_data_cache_save(PET_WIFI_DATA_FEED, NULL, stock));
  assert(!pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &wrong, 1, 0));
  assert(!pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &list, 1, 1));
  const size_t stock_length = stocks.length;
  stocks.length = sizeof(stocks.bytes) + 1;
  assert(!pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &list, 1, 0));
  stocks.length = stock_length - 1;
  assert(!pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &list, 1, 0));
  stocks.length = stock_length;
  assert(pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &list, 1, 0));
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(PET_WIFI_DATA_STOCKS, 0, 1, &view);
  assert(view.received && view.restored && view.stale && view.count == 1);
  assert(pet_p4_widget_data_apply(stock, 2));
  pet_p4_widget_data_view(PET_WIFI_DATA_STOCKS, 0, 2, &view);
  assert(!view.restored && !view.stale);
  assert(pet_p4_widget_data_apply_local(stock, 3, 0));
  assert(!pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &list, 4, 0));
  cJSON_Delete(stock);

  const char story[] = "{\"id\":12,\"type\":\"story\",\"title\":\"Fixture\",\"time\":1794383999}";
  const uint64_t utc = 1794384000000ULL;
  cJSON *headline = pet_wifi_data_snapshot(PET_WIFI_DATA_NEWS, "ok", "", 300000, utc);
  assert(headline && pet_wifi_news_add(headline, story, strlen(story), 12, utc));
  assert(pet_wifi_data_cache_save(PET_WIFI_DATA_NEWS, NULL, headline));
  assert(pet_wifi_data_cache_restore(PET_WIFI_DATA_NEWS, NULL, 5, 0));
  pet_p4_widget_data_view(PET_WIFI_DATA_NEWS, 0, 5, &view);
  assert(view.received && view.restored && view.stale && !strcmp(view.rows[0].id, "12"));
  cJSON_Delete(headline);
}

int main(void) {
  test_clock();
  test_cache();
  return 0;
}
