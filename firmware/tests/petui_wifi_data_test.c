#include "nvs.h"
#include "pet_p4_wifi_data_cache.h"
#include "pet_p4_wifi_feed_http.h"
#include "pet_p4_wifi_data.h"
#include "pet_p4_wifi_http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t settings_revision, clock_ms = 1000;
static pet_wifi_watchlist_t settings;

void pet_wifi_data_set_watchlist(const pet_wifi_watchlist_t *list, uint64_t revision) {
  settings = *list;
  settings_revision = revision;
}

int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }

static void symbols_and_quote_parser(void) {
  pet_wifi_watchlist_t list;
  cJSON *symbols = cJSON_Parse("[\"usAAPL\",\"hk01810\",\"sh600000\"]");
  assert(pet_wifi_watchlist_parse(symbols, &list) && list.count == 3);
  cJSON_Delete(symbols);
  char url[PET_WIFI_DATA_URL_MAX];
  assert(pet_wifi_data_url(url, PET_WIFI_GET_TENCENT_QUOTES, &list, 0));
  assert(!strcmp(url, "https://qt.gtimg.cn/q=usAAPL,hk01810,sh600000"));
  assert(!pet_wifi_data_url(url, PET_WIFI_GET_HN_ITEM, NULL, 0));
  assert(pet_wifi_data_url(url, PET_WIFI_GET_HN_ITEM, NULL, 50344108));
  assert(!strcmp(url, "https://hacker-news.firebaseio.com/v0/item/50344108.json"));
  const char *invalid[] = {"[\"usA?redirect\"]", "[\"usA/../\"]", "[\"usAA,usBB\"]",
                           "[\"hk01810\",\"hk01810\"]", "[\"shabc000\"]", "{}", "[1]"};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    symbols = cJSON_Parse(invalid[i]);
    assert(!pet_wifi_watchlist_parse(symbols, &list));
    cJSON_Delete(symbols);
  }
  strcpy(list.symbols[0], "usA%2FB");
  assert(!pet_wifi_data_url(url, PET_WIFI_GET_TENCENT_QUOTES, &list, 0));
  strcpy(list.symbols[0], "usAAPL");

  char body[1200] = {0};
  size_t used = 0;
  for (int i = 0; i < 3; i++) {
    const char *key = i == 0 ? "hk01810" : i == 1 ? "usAAPL" : "hk09988";
    used += snprintf(body + used, sizeof(body) - used, "\nv_%s=\"100~\xd0\xa1~00000~30.100", key);
    for (int f = 4; f <= 32; f++) {
      const char *value = f == 30 ? (i == 0 ? "2026/11/11 16:08:11" : "20261111120000")
        : f == 32 ? (i == 0 ? "-1.63" : "0.02") : "";
      used += snprintf(body + used, sizeof(body) - used, "~%s", value);
    }
    used += snprintf(body + used, sizeof(body) - used, "~0\";");
  }
  cJSON *snapshot = pet_wifi_stock_snapshot(body, &list, 1794384000000ULL);
  assert(snapshot && pet_p4_widget_data_apply_local(snapshot, 1000, 0));
  cJSON_Delete(snapshot);
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(PET_WIFI_DATA_STOCKS, 0, 2000, &view);
  assert(view.count == 3 && !view.stale && view.age_ms == 1000);
  assert(!strcmp(view.rows[0].id, "usAAPL") && !strcmp(view.rows[0].detail, "+0.02%"));
  assert(!strcmp(view.rows[1].value, "30.100") && !strcmp(view.rows[1].detail, "-1.63%"));
  assert(!strcmp(view.rows[1].meta, "HKD 2026-11-11 16:08:11"));
  assert(!strcmp(view.rows[2].value, "--"));
}

static void fallback_and_settings(void) {
  const char *payload =
      "{\"schema\":1,\"source\":\"stocks.watchlist\",\"ttlMs\":30000,\"status\":\"empty\","
      "\"message\":\"pc\",\"rows\":[]}";
  cJSON *pc = cJSON_Parse(payload);
  assert(pc && pet_p4_widget_data_apply(pc, 31000));
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(PET_WIFI_DATA_STOCKS, 0, 31000, &view);
  assert(view.count == 3); /* A fresh device cache takes priority. */
  pet_p4_widget_data_view(PET_WIFI_DATA_STOCKS, 0, 31001, &view);
  assert(view.count == 0 && !strcmp(view.message, "pc"));
  cJSON_AddItemToObject(pc, "deviceWatchlist", cJSON_Parse("[\"usA?\"]"));
  assert(!pet_p4_widget_data_apply(pc, 32000) && !settings_revision);
  cJSON_ReplaceItemInObject(pc, "deviceWatchlist", cJSON_Parse("[]"));
  assert(pet_p4_widget_data_apply(pc, 32000) && settings_revision == 1 && !settings.count);
  assert(pet_p4_widget_data_apply(pc, 33000) && settings_revision == 1);
  cJSON_DeleteItemFromObject(pc, "deviceWatchlist");
  assert(!pet_p4_widget_data_apply_local(pc, 34000, 0)); /* Old in-flight settings. */
  assert(pet_p4_widget_data_apply_local(pc, 34000, 1));
  pet_p4_widget_data_view(PET_WIFI_DATA_STOCKS, 0, 64001, &view);
  assert(view.stale && view.age_ms == 30001);
  cJSON_Delete(pc);
}

static void news_parser(void) {
  const char top[] = "[12,11,12]";
  uint32_t ids[8];
  unsigned count;
  assert(pet_wifi_news_ids(top, strlen(top), ids, &count) && count == 2 && ids[1] == 11);
  const char oversize[] = "[1,2,3,4,5,6,7,8,9]";
  assert(!pet_wifi_news_ids(oversize, strlen(oversize), ids, &count));
  cJSON *news = pet_wifi_data_snapshot(PET_WIFI_DATA_NEWS, "ok", "", 300000, 1794384000000ULL);
  assert(news);
  const char story[] = "{\"type\":\"story\",\"id\":12,\"time\":1794383999,"
    "\"title\":\"Some very long headline that crosses UTF-8 boundaries: 工程练习 and continuing on\","
    "\"url\":\"https://untrusted.invalid\"}";
  assert(!pet_wifi_news_add(news, story, strlen(story), 11, 1794384000000ULL));
  assert(!pet_wifi_news_add(news, story, strlen(story), 12, 1794383998000ULL));
  assert(!pet_wifi_news_add(news, story, strlen(story), 12, 1794470400000ULL));
  const char dead[] = "{\"type\":\"story\",\"id\":12,\"time\":1794383999,\"title\":\"test\",\"dead\":true}";
  assert(!pet_wifi_news_add(news, dead, strlen(dead), 12, 1794384000000ULL));
  assert(pet_wifi_news_add(news, story, strlen(story), 12, 1794384000000ULL));
  assert(!pet_p4_widget_data_apply(news, 1000)); /* Reserved for native input. */
  assert(pet_p4_widget_data_apply_local(news, 1000, 0));
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(PET_WIFI_DATA_NEWS, 0, 2000, &view);
  assert(view.count == 1 && !view.stale && !strcmp(view.rows[0].id, "12"));
  assert(strstr(view.rows[0].meta, "UTC") && !strstr(view.rows[0].meta, "untrusted"));
  pet_p4_widget_data_view(PET_WIFI_DATA_NEWS, 0, 301001, &view);
  assert(view.stale && view.count == 1);
  cJSON_Delete(news);
}

static void generic_feed_parser_and_cache(void) {
  const char *source = PET_WIFI_DATA_FEED_PREFIX "abcdef";
  const char valid[] = "{\"schema\":1,\"status\":\"ok\",\"message\":\"updated\",\"rows\":["
    "{\"id\":\"temp\",\"label\":\"Temperature\",\"value\":\"21\",\"detail\":\"°C\",\"meta\":\"[,{}]\",\"tone\":0}]} \n";
  cJSON *snapshot = pet_wifi_feed_snapshot(source, valid, strlen(valid), NULL);
  assert(snapshot);
  assert(!pet_p4_widget_data_apply(snapshot, 1000));
  assert(pet_p4_widget_data_apply_local(snapshot, 1000, 0));
  cJSON_Delete(snapshot);
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(source, 0, 2000, &view);
  assert(view.received && !view.stale && view.count == 1 && view.age_ms == 1000);
  assert(!strcmp(view.rows[0].detail, "°C"));
  pet_p4_widget_data_view(PET_WIFI_DATA_FEED_PREFIX "other", 0, 2000, &view);
  assert(!view.received && !view.count);
  pet_p4_widget_data_view(source, 0, 121000, &view);
  assert(!view.stale && view.count == 1);
  pet_p4_widget_data_view(source, 0, 121001, &view);
  assert(view.received && view.stale && !view.count);

  const char empty[] = "{\"schema\":1,\"status\":\"empty\",\"message\":\"\",\"rows\":[]}";
  snapshot = pet_wifi_feed_snapshot(PET_WIFI_DATA_FEED_PREFIX "other", empty, strlen(empty), NULL);
  assert(snapshot && pet_p4_widget_data_apply_local(snapshot, 3000, 0));
  cJSON_Delete(snapshot);
  pet_p4_widget_data_view(source, 0, 3000, &view);
  assert(!view.received);

  const char *rejected[] = {
    "{\"schema\":1,\"status\":\"ok\",\"message\":\"\",\"rows\":[]}{}",
    "{\"schema\":1,\"status\":\"ok\",\"message\":\"\\u0000\",\"rows\":[]}",
    "{\"schema\":1,\"status\":\"ok\",\"message\":\"\xc0\x80\",\"rows\":[]}",
    "{\"schema\":1,\"status\":\"ok\",\"status\":\"ok\",\"message\":\"\",\"rows\":[]}",
    "{\"schema\":1,\"source\":\"stocks.watchlist\",\"status\":\"ok\",\"message\":\"\",\"rows\":[]}",
    "{\"schema\":1,\"ttlMs\":120000,\"status\":\"ok\",\"message\":\"\",\"rows\":[]}",
    "{\"schema\":1,\"status\":\"ok\",\"message\":\"\",\"rows\":[[[]]]}",
    "{\"schema\":1,\"status\":\"ok\",\"message\":\"\",\"rows\":[{\"id\":\"missing fields\"}]}",
  };
  for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++)
    assert(!pet_wifi_feed_snapshot(source, rejected[i], strlen(rejected[i]), NULL));
  assert(!pet_wifi_feed_snapshot(PET_WIFI_DATA_STOCKS, valid, strlen(valid), NULL));
  assert(!pet_wifi_feed_snapshot(source, valid, PET_WIFI_DATA_BODY_MAX + 1, NULL));

  const char bad_row[] = "{\"schema\":1,\"status\":\"ok\",\"message\":\"\",\"rows\":["
    "{\"id\":\"temp\",\"label\":\"Temperature\",\"value\":21,\"detail\":\"°C\",\"meta\":\"\",\"tone\":0}]}";
  snapshot = pet_wifi_feed_snapshot(source, bad_row, strlen(bad_row), NULL);
  assert(snapshot && !pet_p4_widget_data_apply_local(snapshot, 4000, 0));
  cJSON_Delete(snapshot);

  char full[4096];
  size_t used = snprintf(full, sizeof(full), "{\"schema\":1,\"status\":\"ok\",\"message\":\"\",\"rows\":[");
  for (unsigned i = 0; i < PET_P4_DATA_MAX_ROWS; i++) {
    used += snprintf(full + used, sizeof(full) - used, "%s{\"id\":\"%u\",\"label\":\"L\","
      "\"value\":\"1\",\"detail\":\"\",\"meta\":\"\",\"tone\":0}", i ? "," : "", i);
  }
  strcpy(full + used, "]}");
  snapshot = pet_wifi_feed_snapshot(source, full, used + 2, NULL);
  assert(snapshot && pet_p4_widget_data_apply_local(snapshot, 4000, 0));
  cJSON_Delete(snapshot);
  clock_ms = 124000;
  assert(pet_p4_widget_data_paging(source, 1).pages == 4);
  clock_ms++;
  assert(pet_p4_widget_data_paging(source, 1).pages == 1);
  clock_ms = 1000;
  strcpy(full + used, ",{}]}");
  assert(!pet_wifi_feed_snapshot(source, full, used + 5, NULL));
}

static void mapped_feed(void) {
  const char *source = PET_WIFI_DATA_FEED_PREFIX "mapped";
  const char *declaration = "{\"rows\":\"data.items\",\"id\":\"record.id\",\"label\":\"record.Name\","
    "\"value\":\"v\",\"meta\":\"active\",\"tone\":\"signal\"}";
  const char *body = "{\"data\":{\"items\":[{\"record\":{\"id\":7,\"Name\":\"室外\"},"
    "\"v\":21.5,\"active\":true,\"signal\":-1}]},\"ttlMs\":1,\"status\":\"error\"}";
  pet_wifi_feed_map_t map;
  cJSON *config = cJSON_Parse(declaration);
  assert(config && pet_wifi_feed_map_parse(config, &map));
  cJSON_Delete(config);
  cJSON *snapshot = pet_wifi_feed_snapshot(source, body, strlen(body), &map);
  assert(snapshot && pet_p4_widget_data_apply_local(snapshot, 5000, 0));
  assert(cJSON_GetObjectItemCaseSensitive(snapshot, "ttlMs")->valueint == PET_WIFI_DATA_FEED_TTL_MS);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snapshot, "clearOnExpire")));
  cJSON_Delete(snapshot);
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(source, 0, 5000, &view);
  assert(!strcmp(view.status, "ok") && view.count == 1);
  assert(!strcmp(view.rows[0].id, "7") && !strcmp(view.rows[0].label, "室外"));
  assert(!strcmp(view.rows[0].value, "21.5") && !strcmp(view.rows[0].meta, "true"));
  assert(!view.rows[0].detail[0] && view.rows[0].tone == -1);

  const char *invalid_maps[] = {
    "null", "[]", "{\"rows\":\"$\"}", "{\"rows\":\"$\",\"id\":\"$\",\"label\":\"name\"}",
    "{\"rows\":\"$\",\"id\":\"a[0]\",\"label\":\"name\"}",
    "{\"rows\":\"$\",\"id\":\"a..b\",\"label\":\"name\"}",
    "{\"rows\":\"$\",\"id\":\"a.b.c.d.e\",\"label\":\"name\"}",
    "{\"rows\":\"$\",\"id\":\"id\",\"id\":\"other\",\"label\":\"name\"}",
    "{\"rows\":\"$\",\"id\":\"id\",\"label\":\"name\",\"ttlMs\":\"ttl\"}",
  };
  for (size_t i = 0; i < sizeof(invalid_maps) / sizeof(invalid_maps[0]); i++) {
    config = cJSON_Parse(invalid_maps[i]);
    assert(config && !pet_wifi_feed_map_parse(config, &map));
    cJSON_Delete(config);
  }

  config = cJSON_Parse("{\"rows\":\"$\",\"id\":\"id\",\"label\":\"name\"}");
  assert(config && pet_wifi_feed_map_parse(config, &map));
  cJSON_Delete(config);
  snapshot = pet_wifi_feed_snapshot(source, "[]", 2, &map);
  assert(snapshot && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(snapshot, "status")), "empty"));
  cJSON_Delete(snapshot);
  const char *invalid_bodies[] = {
    "[{\"id\":1,\"Name\":\"wrong case\"}]",
    "[{\"id\":1,\"id\":2,\"name\":\"duplicate key\"}]",
    "[{\"id\":1.5,\"name\":\"fractional ID\"}]",
    "[{\"id\":9007199254740992,\"name\":\"large ID\"}]",
    "[{\"id\":1,\"name\":null}]",
    "[{\"id\":1,\"name\":\"a name longer than thirty-six bytes!!\"}]",
    "[{\"id\":1,\"name\":\"NUL \\u0000\"}]",
    "[{\"id\":1,\"name\":\"x\",\"extra\":[[[[[[[[0]]]]]]]]}]",
    "[] true",
  };
  for (size_t i = 0; i < sizeof(invalid_bodies) / sizeof(invalid_bodies[0]); i++)
    assert(!pet_wifi_feed_snapshot(source, invalid_bodies[i], strlen(invalid_bodies[i]), &map));
  const char *duplicate_ids = "[{\"id\":1,\"name\":\"x\"},{\"id\":\"1\",\"name\":\"y\"}]";
  snapshot = pet_wifi_feed_snapshot(source, duplicate_ids, strlen(duplicate_ids), &map);
  assert(snapshot && !pet_p4_widget_data_apply_local(snapshot, 6000, 0));
  cJSON_Delete(snapshot);

  char oversized[2048];
  size_t used = 0;
  oversized[used++] = '[';
  for (unsigned i = 0; i <= PET_P4_DATA_MAX_ROWS; i++)
    used += snprintf(oversized + used, sizeof(oversized) - used,
                     "%s{\"id\":%u,\"name\":\"x\"}", i ? "," : "", i);
  oversized[used++] = ']';
  assert(!pet_wifi_feed_snapshot(source, oversized, used, &map));
  used = snprintf(oversized, sizeof(oversized), "[{\"id\":1,\"name\":\"x\",\"extra\":[0");
  for (unsigned i = 0; i < 512; i++) {
    oversized[used++] = ',';
    oversized[used++] = '0';
  }
  memcpy(oversized + used, "]}]", 3);
  assert(!pet_wifi_feed_snapshot(source, oversized, used + 3, &map));
  assert(!pet_wifi_feed_snapshot(source, oversized, PET_WIFI_DATA_BODY_MAX + 1, &map));
}

typedef struct {
  const char *cursor;
  size_t remaining;
} http_input_t;

static bool http_read(void *ctx, char *out, size_t bytes) {
  http_input_t *input = ctx;
  if (bytes > input->remaining) return false;
  memcpy(out, input->cursor, bytes);
  input->cursor += bytes;
  input->remaining -= bytes;
  return true;
}

static void bounded_http_response(void) {
  const char *accepted[] = {
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n2;x=y\r\nhe\r\n3\r\nllo\r\n0\r\nServer-Timing: x\r\n\r\n",
  };
  const char *rejected[] = {
    "HTTP/1.1 302 Found\r\nContent-Length: 5\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nhelloo",
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhell",
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Encoding: gzip\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nhel\r\n3\r\nloo\r\n0\r\n\r\n",
    "HTTP/1.1 200 OK\r\n\r\nhello",
  };
  char body[6];
  size_t length;
  for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
    http_input_t input = {accepted[i], strlen(accepted[i])};
    assert(pet_wifi_http_body(http_read, &input, body, 5, &length));
    assert(length == 5 && !strcmp(body, "hello"));
  }
  for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
    http_input_t input = {rejected[i], strlen(rejected[i])};
    assert(!pet_wifi_http_body(http_read, &input, body, 5, &length));
  }
}

static void feed_http_options(void) {
  pet_wifi_feed_http_t config;
  assert(pet_wifi_feed_http_parse(NULL, &config) && !config.accept && !config.revalidate && !config.persist);
  cJSON *valid = cJSON_Parse("{\"accept\":\"application/geo+json\",\"revalidate\":true,\"cache\":\"last-good\"}");
  assert(valid && pet_wifi_feed_http_parse(valid, &config));
  assert(!strcmp(pet_wifi_feed_media_types[config.accept], "application/geo+json"));
  assert(config.revalidate && config.persist);
  cJSON_Delete(valid);
  const char *invalid[] = {"null", "[]", "{}", "{\"method\":\"POST\"}", "{\"authorization\":\"key\"}",
    "{\"accept\":\"application/json\\r\\nHost: bad\"}", "{\"revalidate\":1}", "{\"cache\":true}",
    "{\"cache\":\"last-good\",\"cache\":\"last-good\"}"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
    cJSON *bad = cJSON_Parse(invalid[i]);
    assert(bad && !pet_wifi_feed_http_parse(bad, &config));
    cJSON_Delete(bad);
  }
}

static void conditional_http(void) {
  const char initial[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nETag: W/\"v1\"\r\n\r\nhello";
  const char unchanged[] = "HTTP/1.1 304 Not Modified\r\nContent-Length: 5000\r\nETag: \"v1\"\r\n\r\n";
  const char wrong[] = "HTTP/1.1 304 Not Modified\r\nETag: \"v2\"\r\n\r\n";
  const char restricted[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nETag: \"v1\"\r\n"
    "Cache-Control: max-age=0, Private\r\nCache-Control: no-cache\r\n\r\nhello";
  const char duplicate[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nETag: \"v1\"\r\nETag: \"v2\"\r\n\r\nhello";
  char body[6];
  size_t length;
  pet_wifi_http_metadata_t metadata;
  http_input_t input = {initial, strlen(initial)};
  assert(pet_wifi_http_response(http_read, &input, body, 5, &length, NULL, &metadata));
  assert(length == 5 && !strcmp(metadata.etag, "W/\"v1\"") && !metadata.no_store);
  input = (http_input_t){unchanged, strlen(unchanged)};
  assert(pet_wifi_http_response(http_read, &input, body, 5, &length, "W/\"v1\"", &metadata));
  assert(!length && !input.remaining);
  input = (http_input_t){unchanged, strlen(unchanged)};
  assert(!pet_wifi_http_body(http_read, &input, body, 5, &length));
  input = (http_input_t){wrong, strlen(wrong)};
  assert(!pet_wifi_http_response(http_read, &input, body, 5, &length, "\"v1\"", &metadata));
  input = (http_input_t){restricted, strlen(restricted)};
  assert(pet_wifi_http_response(http_read, &input, body, 5, &length, NULL, &metadata));
  assert(metadata.no_store);
  input = (http_input_t){duplicate, strlen(duplicate)};
  assert(pet_wifi_http_response(http_read, &input, body, 5, &length, NULL, &metadata));
  assert(!metadata.etag[0]);
  assert(!pet_wifi_http_valid_etag("*") && !pet_wifi_http_valid_etag("\"x\"\r\nHost: invalid"));
}

static char persisted_feed[4096];
static size_t persisted_feed_size;

int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
  assert(!strcmp(name, "pet_wifi_cache"));
  *handle = mode;
  return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
int nvs_commit(nvs_handle_t handle) { return handle == NVS_READWRITE ? ESP_OK : -1; }
int nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *length) {
  assert(handle == NVS_READONLY && !strcmp(key, "feed_v1"));
  if (!persisted_feed_size || (bytes && *length < persisted_feed_size)) return -1;
  *length = persisted_feed_size;
  if (bytes) memcpy(bytes, persisted_feed, *length);
  return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t length) {
  assert(handle == NVS_READWRITE && !strcmp(key, "feed_v1") && length <= sizeof(persisted_feed));
  memcpy(persisted_feed, bytes, length);
  persisted_feed_size = length;
  return ESP_OK;
}

static void feed_offline_cache(void) {
  const char *first = PET_WIFI_DATA_FEED_PREFIX "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *second = PET_WIFI_DATA_FEED_PREFIX "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char body[] = "{\"schema\":1,\"status\":\"ok\",\"message\":\"\",\"rows\":["
    "{\"id\":\"1\",\"label\":\"Reading\",\"value\":\"1\",\"detail\":\"\",\"meta\":\"\",\"tone\":0}]}";
  cJSON *a = pet_wifi_feed_snapshot(first, body, strlen(body), NULL);
  cJSON *b = pet_wifi_feed_snapshot(second, body, strlen(body), NULL);
  assert(a && b && !pet_wifi_data_cache_save(first, NULL, a)); /* Legacy/feed without opt-in. */
  cJSON_DeleteItemFromObjectCaseSensitive(a, "clearOnExpire");
  cJSON_DeleteItemFromObjectCaseSensitive(b, "clearOnExpire");
  assert(pet_wifi_data_cache_save(first, NULL, a));
  assert(!pet_wifi_data_cache_restore(second, NULL, 800, 0));
  assert(pet_p4_widget_data_apply_local(b, 800, 0));
  assert(pet_wifi_data_cache_restore(first, NULL, 900, 0));
  pet_p4_data_view_t view;
  pet_p4_widget_data_view(first, 0, 900, &view);
  assert(view.received && view.restored && view.stale && view.count == 1);
  assert(!pet_p4_widget_data_revalidate_feed(first, 1000)); /* No live validator after boot. */
  assert(pet_p4_widget_data_apply_local(a, 1000, 0));
  assert(!pet_wifi_data_cache_restore(first, NULL, 1001, 0)); /* Never overwrite live data. */
  assert(!pet_p4_widget_data_revalidate_feed(second, 121001));
  assert(pet_p4_widget_data_revalidate_feed(first, 121001));
  pet_p4_widget_data_view(first, 0, 121002, &view);
  assert(!view.stale && !view.restored && view.count == 1);
  assert(pet_wifi_data_cache_save(second, NULL, b)); /* Single shared slot, exact identity. */
  assert(!pet_wifi_data_cache_restore(first, NULL, 121002, 0));
  cJSON_Delete(a);
  cJSON_Delete(b);
}

int main(void) {
  symbols_and_quote_parser();
  fallback_and_settings();
  news_parser();
  generic_feed_parser_and_cache();
  mapped_feed();
  bounded_http_response();
  feed_http_options();
  conditional_http();
  feed_offline_cache();
  puts("WiFi data sources, settings generations, feed isolation and cache fallback passed");
  return 0;
}
