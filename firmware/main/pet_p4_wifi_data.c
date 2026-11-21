#include "pet_p4_wifi_data.h"

#if PET_P4_SYSTEM_WIFI
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "pet_p4_mqtt_feed.h"
#include "pet_p4_mqtt_wire.h"
#include "nvs.h"
#include "pet_p4_device_wifi.h"
#include "pet_p4_live_tls.h"
#include "pet_p4_protocol.h"
#include "pet_p4_wifi_clock.h"
#include "pet_p4_wifi_data_cache.h"
#include "pet_p4_wifi_data_roots.h"
#include "pet_p4_wifi_http.h"

#define STOCK_INTERVAL_MS 15000
/* Persist the first good snapshot, then checkpoint periodically to limit NVS wear. */
#define CACHE_SAVE_INTERVAL_MS 900000
#define CACHE_RETRY_INTERVAL_MS 30000
#define NEWS_INTERVAL_MS 240000
#define FEED_INTERVAL_MS 60000
#define MANUAL_INTERVAL_MS 10000
#define HTTP_ATTEMPT_MS 5000
#define NEWS_BUDGET_MS 20000
#define RETRY_MAX_MS 300000
#define MIN_TLS_EPOCH 1704067200

typedef enum { SOURCE_NONE, SOURCE_STOCK, SOURCE_NEWS, SOURCE_FEED, SOURCE_COUNT } source_t;
typedef struct {
  uint64_t due;
  uint32_t failures;
} schedule_t;
typedef struct {
  esp_tls_t *tls;
  uint64_t deadline;
  size_t cursor, available;
  pet_live_tls_current_fn current;
  void *context;
} https_reader_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t worker;
static source_t visible;
static atomic_bool retrying[SOURCE_COUNT];
static bool pending, refresh_requested;
static uint64_t visible_generation;
static EXT_RAM_BSS_ATTR pet_wifi_data_feed_t visible_feed;
static bool needs_account;
static uint32_t account_epoch;
bool pet_wifi_data_authorization_needed(const pet_private_feed_t *binding) {
  portENTER_CRITICAL(&lock);
  bool needed = needs_account && visible == SOURCE_FEED &&
    account_epoch == pet_private_storage_epoch() &&
    (!binding || !memcmp(binding->id, visible_feed.private.id, sizeof(binding->id)));
  portEXIT_CRITICAL(&lock);
  return needed;
}
static EXT_RAM_BSS_ATTR pet_wifi_watchlist_t pending_list;
/* Only the Wi-Fi data worker uses this buffer. */
static EXT_RAM_BSS_ATTR char receive_buffer[1024];
static uint64_t pending_revision;

static uint64_t now_ms(void) {
  return (uint64_t)esp_timer_get_time() / 1000;
}

bool pet_wifi_data_feed_init(pet_wifi_data_feed_t *out, const char *component_id, const char *url,
                             const cJSON *response_map, const cJSON *http, const cJSON *connection) {
  pet_wifi_data_feed_t feed = {0};
  if (!out || !component_id || !*component_id ||
      (connection ? (http || !pet_private_feed_parse(&feed.private, &feed.endpoint, component_id, url, connection))
                  : !pet_wifi_endpoint_parse(url, &feed.endpoint)) ||
      !pet_wifi_feed_map_parse(response_map, &feed.response_map) ||
      !pet_wifi_feed_http_parse(http, &feed.http)) return false;
  char identity[sizeof(feed.source) + sizeof(feed.endpoint) + 6 +
                sizeof(feed.response_map.paths) + sizeof(feed.private.id) + 1];
  if (strlen(component_id) >= sizeof(feed.source)) return false;
  /* Component ids cannot contain newlines. Include the normalized URL origin
   * and the exact request target so edits get a separate cache identity. */
  int length = snprintf(identity, sizeof(identity), "%s\n%s\n%s",
    component_id, feed.endpoint.host, feed.endpoint.path);
  if (length <= 0 || (size_t)length >= sizeof(identity) - sizeof(feed.response_map.paths)) return false;
  if (response_map) {
    /* Include the NUL separator and canonical field order in mapped feed identities. */
    memcpy(identity + length + 1, feed.response_map.paths, sizeof(feed.response_map.paths));
    length += 1 + sizeof(feed.response_map.paths);
  }
  if (http) {
    /* Preserve legacy GET keys. A different representation or cache policy gets a new key. */
    identity[length++] = 0;
    identity[length++] = feed.http.accept;
    identity[length++] = feed.http.revalidate;
    identity[length++] = feed.http.persist;
  }
  if (connection) {
    identity[length++] = 0;
    memcpy(identity + length, feed.private.id, sizeof(feed.private.id));
    length += sizeof(feed.private.id);
  }
  unsigned char hash[32];
  if (mbedtls_sha256((const unsigned char *)identity, (size_t)length, hash, 0) != 0) return false;
  static const char hex[] = "0123456789abcdef";
  strcpy(feed.source, PET_WIFI_DATA_FEED_PREFIX);
  char *key = feed.source + sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1;
  for (unsigned i = 0; i < 16; i++) {
    *key++ = hex[hash[i] >> 4];
    *key++ = hex[hash[i] & 15];
  }
  *key = 0;
  *out = feed;
  return true;
}

static source_t source_id(const char *source) {
  if (source && !strcmp(source, PET_WIFI_DATA_STOCKS)) return SOURCE_STOCK;
  if (source && !strcmp(source, PET_WIFI_DATA_NEWS)) return SOURCE_NEWS;
  if (source && !strncmp(source, PET_WIFI_DATA_FEED_PREFIX, sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1))
    return SOURCE_FEED;
  return SOURCE_NONE;
}

static bool request_connect_current(void *ctx) {
  (void)ctx;
  return pet_device_wifi_online();
}

typedef struct {
  uint64_t generation;
  uint32_t credential_epoch;
  const pet_wifi_data_feed_t *feed;
} feed_run_t;

static bool feed_current(void *ctx) {
  const feed_run_t *run = ctx;
  portENTER_CRITICAL(&lock);
  bool current = visible == SOURCE_FEED && visible_generation == run->generation;
  portEXIT_CRITICAL(&lock);
  return current && pet_device_wifi_online() &&
    (!run->feed->private.kind || run->credential_epoch == pet_private_storage_epoch());
}

static bool tls_waitable(ssize_t result) {
  return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

static void request_yield(void) {
  vTaskDelay(pdMS_TO_TICKS(10) + 1);
}

static bool tls_read_exact(void *ctx, char *out, size_t bytes) {
  https_reader_t *reader = ctx;
  while (bytes && now_ms() < reader->deadline && reader->current(reader->context)) {
    if (reader->cursor == reader->available) {
      ssize_t n = esp_tls_conn_read(reader->tls, receive_buffer, sizeof(receive_buffer));
      if (tls_waitable(n)) {
        request_yield();
        continue;
      }
      if (n <= 0) return false;
      reader->cursor = 0;
      reader->available = (size_t)n;
    }
    size_t count = reader->available - reader->cursor;
    if (count > bytes) count = bytes;
    memcpy(out, receive_buffer + reader->cursor, count);
    reader->cursor += count;
    out += count;
    bytes -= count;
  }
  return bytes == 0;
}

static bool tls_send_request(const https_reader_t *reader, const char *host, const char *path,
                             const char *accept, const char *etag, const char *bearer) {
  char request[PET_WIFI_DATA_URL_MAX + 176 + PET_WIFI_HTTP_ETAG_SIZE + PET_PRIVATE_TOKEN_MAX];
  int length = snprintf(request, sizeof(request),
    "GET %s HTTP/1.1\r\nHost: %s\r\nAccept-Encoding: identity\r\nConnection: close\r\n"
    "%s%s%s%s%s%s%s%s%s\r\n", path, host,
    accept ? "Accept: " : "", accept ? accept : "", accept ? "\r\n" : "",
    etag ? "If-None-Match: " : "", etag ? etag : "", etag ? "\r\n" : "",
    bearer ? "Authorization: Bearer " : "", bearer ? bearer : "", bearer ? "\r\n" : "");
  if (length <= 0 || (size_t)length >= sizeof(request)) {
    mbedtls_platform_zeroize(request, sizeof(request));
    return false;
  }
  int sent = 0;
  while (sent < length && now_ms() < reader->deadline && reader->current(reader->context)) {
    ssize_t n = esp_tls_conn_write(reader->tls, request + sent, (size_t)(length - sent));
    if (n > 0) sent += (int)n;
    else if (tls_waitable(n)) request_yield();
    else break;
  }
  mbedtls_platform_zeroize(request, sizeof(request));
  return sent == length;
}

static bool read_response(https_reader_t *reader, const char *host, const char *path,
                          char *body, size_t limit, size_t *length, const char *accept,
                          const char *etag, pet_wifi_http_metadata_t *metadata, const char *bearer) {
  if (!reader->tls) return false;
  bool ok = tls_send_request(reader, host, path, accept, etag, bearer)
    && pet_wifi_http_response(tls_read_exact, reader, body, limit, length, etag, metadata)
    && now_ms() < reader->deadline && reader->current(reader->context);
  if (ok) pet_device_wifi_note_https(PET_WIFI_HTTPS_READY, 0);
  esp_tls_conn_destroy(reader->tls);
  return ok;
}

static bool request(pet_wifi_data_endpoint_t endpoint, const pet_wifi_watchlist_t *list, uint32_t item,
                    char *body, size_t limit, uint64_t budget_end, size_t *length) {
  char url[PET_WIFI_DATA_URL_MAX];
  uint64_t now = now_ms();
  if (now >= budget_end || !pet_device_wifi_online() || !pet_wifi_data_url(url, endpoint, list, item))
    return false;
  uint64_t deadline = now + HTTP_ATTEMPT_MS;
  if (deadline > budget_end) deadline = budget_end;
  bool tencent = endpoint == PET_WIFI_GET_TENCENT_QUOTES;
  const char *host = tencent ? "qt.gtimg.cn" : "hacker-news.firebaseio.com";
  size_t host_length = strlen(host);
  const char *authority = url + sizeof("https://") - 1;
  if (strncmp(url, "https://", sizeof("https://") - 1) ||
      strncmp(authority, host, host_length) || authority[host_length] != '/') return false;
  const esp_tls_cfg_t config = {
    .non_block = true,
    .timeout_ms = (int)(deadline - now),
    .cacert_buf = tencent ? pet_wifi_ca_tencent_g5 : pet_wifi_ca_hn_r1,
    .cacert_bytes = tencent ? sizeof(pet_wifi_ca_tencent_g5) : sizeof(pet_wifi_ca_hn_r1),
  };
  https_reader_t reader = {.deadline = deadline, .current = request_connect_current};
  reader.tls = pet_live_tls_connect(host, &config, (int64_t)deadline, request_connect_current, NULL);
  return read_response(&reader, host, authority + host_length, body, limit, length, NULL, NULL, NULL, NULL);
}

static bool fetch_feed(feed_run_t *run, char *body, size_t *length, const char *etag,
                       pet_wifi_http_metadata_t *metadata, const char *bearer) {
  const pet_wifi_data_feed_t *feed = run->feed;
  const esp_tls_cfg_t config = {.non_block = true, .timeout_ms = HTTP_ATTEMPT_MS};
  https_reader_t reader = {
    .deadline = now_ms() + HTTP_ATTEMPT_MS, .current = feed_current, .context = run,
  };
  reader.tls = pet_live_tls_connect_public(feed->endpoint.host, &config,
    (int64_t)reader.deadline, reader.current, reader.context);
  return read_response(&reader, feed->endpoint.host, feed->endpoint.path, body,
    PET_WIFI_DATA_BODY_MAX, length, pet_wifi_feed_media_types[feed->http.accept], etag, metadata, bearer);
}

static bool still_visible(source_t source) {
  portENTER_CRITICAL(&lock);
  bool current = visible == source;
  portEXIT_CRITICAL(&lock);
  return current;
}

static cJSON *fetch_news(char *body, uint64_t utc, uint64_t budget_end) {
  uint32_t ids[8];
  unsigned count, kept = 0;
  size_t length;
  if (!request(PET_WIFI_GET_HN_TOP, NULL, 0, body, 2048, budget_end, &length)
      || !pet_wifi_news_ids(body, length, ids, &count)) return NULL;
  cJSON *snapshot = pet_wifi_data_snapshot(PET_WIFI_DATA_NEWS, "ok", "", 300000, utc);
  if (!snapshot) return NULL;
  for (unsigned i = 0; i < count && kept < PET_P4_DATA_PAGE_ROWS
       && now_ms() < budget_end && still_visible(SOURCE_NEWS); i++) {
    if (request(PET_WIFI_GET_HN_ITEM, NULL, ids[i], body, 8192, budget_end, &length)
        && pet_wifi_news_add(snapshot, body, length, ids[i], utc)) kept++;
  }
  if (kept) return snapshot;
  cJSON_Delete(snapshot);
  return NULL;
}

/* Only strings are stored, as bounded JSON with a terminator. Old/malformed
 * settings leave the built-in defaults. Failed writes can retry on PC updates. */
static bool load_watchlist(pet_wifi_watchlist_t *list) {
  nvs_handle_t handle;
  if (nvs_open("pet_wifi_data", NVS_READONLY, &handle) != ESP_OK) return false;
  char raw[PET_WIFI_DATA_URL_MAX];
  size_t length = sizeof(raw);
  esp_err_t result = nvs_get_str(handle, "watchlist", raw, &length);
  nvs_close(handle);
  if (result != ESP_OK || length < 3 || length > sizeof(raw)) return false;
  cJSON *symbols = cJSON_ParseWithLengthOpts(raw, length, NULL, true);
  bool ok = pet_wifi_watchlist_parse(symbols, list);
  cJSON_Delete(symbols);
  return ok;
}

static bool save_watchlist(const pet_wifi_watchlist_t *list) {
  cJSON *symbols = cJSON_CreateArray();
  if (!symbols) return false;
  for (unsigned i = 0; i < list->count; i++) {
    cJSON *symbol = cJSON_CreateString(list->symbols[i]);
    if (!symbol || !cJSON_AddItemToArray(symbols, symbol)) {
      cJSON_Delete(symbol);
      cJSON_Delete(symbols);
      return false;
    }
  }
  char *raw = cJSON_PrintUnformatted(symbols);
  cJSON_Delete(symbols);
  if (!raw) return false;
  nvs_handle_t handle;
  bool ok = false;
  if (nvs_open("pet_wifi_data", NVS_READWRITE, &handle) == ESP_OK) {
    ok = nvs_set_str(handle, "watchlist", raw) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
  }
  cJSON_free(raw);
  return ok;
}

static void accept_mqtt_value(const char *body, size_t length, void *ctx) {
  feed_run_t *run = ctx;
  cJSON *snapshot = pet_wifi_feed_snapshot(run->feed->source, body, length, &run->feed->response_map);
  if (snapshot && feed_current(run)) (void)pet_p4_widget_data_apply_local(snapshot, now_ms(), 0);
  if (!feed_current(run)) pet_p4_widget_data_clear_feed(run->feed->source);
  cJSON_Delete(snapshot);
}

static void wifi_data_worker(void *unused) {
  (void)unused;
  pet_wifi_watchlist_t list = {.count = 2, .symbols = {"hk01810", "hk09988"}};
  bool persisted = load_watchlist(&list);
  uint64_t save_after = 0, list_revision = 0;
  schedule_t schedule[SOURCE_COUNT] = {0};
  bool stock_restored = false, news_restored = false;
  uint64_t stock_cache_after = 0, news_cache_after = 0;
  char scheduled_feed[sizeof(visible_feed.source)] = {0};
  uint32_t last_credential_epoch = 0;
  uint64_t next_feed_attempt = 0, feed_cache_after = 0;
  char feed_etag[PET_WIFI_HTTP_ETAG_SIZE] = {0};
  char *body = NULL;
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    pet_wifi_watchlist_t update;
    pet_wifi_data_feed_t feed;
    portENTER_CRITICAL(&lock);
    source_t source = visible;
    uint64_t generation = visible_generation;
    if (source == SOURCE_FEED) feed = visible_feed;
    bool updated = pending, manual = refresh_requested;
    refresh_requested = false;
    if (updated) {
      update = pending_list;
      list_revision = pending_revision;
      pending = false;
    }
    portEXIT_CRITICAL(&lock);
    uint64_t now = now_ms();
    if (updated) schedule[SOURCE_STOCK] = (schedule_t){0};
    if (updated && !pet_wifi_watchlist_equal(&list, &update)) {
      stock_restored = false;
      stock_cache_after = 0;
      list = update;
      persisted = false;
      save_after = 0;
    }
    if (list_revision && !persisted && now >= save_after) {
      persisted = save_watchlist(&list);
      save_after = now + 30000;
    }
    /* Restore before connectivity and SNTP gates, including on an offline boot. */
    if (source == SOURCE_STOCK && (!stock_restored || updated)) {
      stock_restored = true;
      pet_wifi_data_cache_restore(PET_WIFI_DATA_STOCKS, &list, now, list_revision);
    } else if (source == SOURCE_NEWS && !news_restored) {
      news_restored = true;
      pet_wifi_data_cache_restore(PET_WIFI_DATA_NEWS, NULL, now, 0);
    }
    if (source == SOURCE_NONE) continue;
    schedule_t *job = &schedule[source];
    feed_run_t run = {.generation = generation, .credential_epoch = pet_private_storage_epoch(), .feed = &feed};
    if (source == SOURCE_FEED && (strcmp(feed.source, scheduled_feed) ||
        (feed.private.kind && run.credential_epoch != last_credential_epoch))) {
      if (feed.private.kind) pet_p4_widget_data_clear_feed(feed.source);
      last_credential_epoch = run.credential_epoch;
      *job = (schedule_t){0};
      strcpy(scheduled_feed, feed.source);
      feed_etag[0] = 0;
      if (feed.http.persist) pet_wifi_data_cache_restore(feed.source, NULL, now, 0);
    }
    /* A user refresh respects a short minimum interval and failure backoff. */
    if (manual && !job->failures && job->due > now + MANUAL_INTERVAL_MS)
      job->due = now + MANUAL_INTERVAL_MS;
    uint64_t utc, age;
    int32_t offset;
    if (now < job->due || (source == SOURCE_FEED && now < next_feed_attempt) || !pet_device_wifi_online()
        || !pet_wifi_clock_read(now, &utc, &offset, &age) || age > 86400000ULL
        || time(NULL) < MIN_TLS_EPOCH) continue;
    if (source == SOURCE_FEED) {
      if (!feed_current(&run)) continue;
      next_feed_attempt = now + MANUAL_INTERVAL_MS;
    }
    if (!body) body = heap_caps_malloc(PET_MQTT_BODY_LIMIT + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    atomic_store(&retrying[source], false);
    cJSON *snapshot = NULL;
    pet_wifi_http_metadata_t metadata = {0};
    bool unchanged = false;
    bool feed_response = false;
    if (body) {
      size_t length;
      if (source == SOURCE_FEED) {
        pet_private_secret_t secret = {0};
        pet_private_result_t credential = pet_feed_requires_account(feed.private.kind)
          ? pet_private_secret_read(&feed.private, &secret) : PET_PRIVATE_SET;
        portENTER_CRITICAL(&lock);
        if (generation == visible_generation) {
          needs_account = credential == PET_PRIVATE_ABSENT;
          account_epoch = run.credential_epoch;
        }
        portEXIT_CRITICAL(&lock);
        if (credential != PET_PRIVATE_SET) {
          const char *message = credential == PET_PRIVATE_ABSENT ? "请连接服务账号" :
            credential == PET_PRIVATE_NO_STORAGE ? "设备安全存储未配置" : "无法读取设备凭证";
          snapshot = pet_wifi_data_snapshot(feed.source, "error", message, 300000, 0);
        } else if ((feed.private.kind == PET_PRIVATE_MQTT || feed.private.kind == PET_PUBLIC_MQTT)) {
          if (pet_mqtt_feed_run(&feed.endpoint, &feed.private, &secret, body,
              feed_current, accept_mqtt_value, &run)) job->failures = 0;
          pet_p4_widget_data_clear_feed(feed.source);
        } else {
          const bool bearer = feed.private.kind == PET_PRIVATE_BEARER;
          feed_response = fetch_feed(&run, body, &length, feed_etag[0] ? feed_etag : NULL,
            &metadata, bearer ? secret.token : NULL);
          if (feed_response && length)
            snapshot = pet_wifi_feed_snapshot(feed.source, body, length, &feed.response_map);
          unchanged = feed_response && !length;
          if (bearer) mbedtls_platform_zeroize(body, PET_WIFI_DATA_BODY_MAX + 1);
        }
        mbedtls_platform_zeroize(&secret, sizeof(secret));
      }
      else if (source == SOURCE_NEWS) snapshot = fetch_news(body, utc, now + NEWS_BUDGET_MS);
      else if (!list.count) {
        body[0] = 0;
        snapshot = pet_wifi_stock_snapshot(body, &list, utc);
      }
      else if (request(PET_WIFI_GET_TENCENT_QUOTES, &list, 0, body, PET_WIFI_DATA_BODY_MAX,
                       now + HTTP_ATTEMPT_MS, &length)) snapshot = pet_wifi_stock_snapshot(body, &list, utc);
    }
    now = now_ms();
    if (source == SOURCE_FEED && !feed_current(&run)) {
      cJSON_Delete(snapshot);
      continue;
    }
    const bool good_feed = source == SOURCE_FEED && pet_wifi_data_cache_good_feed(snapshot);
    const bool keep_feed = good_feed && feed.http.persist && !metadata.no_store;
    if (keep_feed) cJSON_DeleteItemFromObjectCaseSensitive(snapshot, "clearOnExpire");
    bool ok = snapshot && (source != SOURCE_FEED || !feed.http.persist || good_feed)
      && pet_p4_widget_data_apply_local(snapshot, now, list_revision);
    if (source == SOURCE_FEED && feed.private.kind && !feed_current(&run)) {
      pet_p4_widget_data_clear_feed(feed.source);
      cJSON_Delete(snapshot);
      continue;
    }
    if (source == SOURCE_FEED && (feed_response || feed_etag[0])) {
      if (unchanged && !metadata.no_store) ok = pet_p4_widget_data_revalidate_feed(feed.source, now);
      if (!ok || metadata.no_store) feed_etag[0] = 0;
      else if (!unchanged && feed.http.revalidate)
        strcpy(feed_etag, good_feed ? metadata.etag : "");
    }
    if (ok && source == SOURCE_STOCK && list.count && (persisted || !list_revision) && now >= stock_cache_after) {
      const bool saved = pet_wifi_data_cache_save(PET_WIFI_DATA_STOCKS, &list, snapshot);
      stock_cache_after = now + (saved ? CACHE_SAVE_INTERVAL_MS : CACHE_RETRY_INTERVAL_MS);
    } else if (ok && source == SOURCE_NEWS && now >= news_cache_after) {
      const bool saved = pet_wifi_data_cache_save(PET_WIFI_DATA_NEWS, NULL, snapshot);
      news_cache_after = now + (saved ? CACHE_SAVE_INTERVAL_MS : CACHE_RETRY_INTERVAL_MS);
    } else if (ok && keep_feed && now >= feed_cache_after) {
      const bool saved = pet_wifi_data_cache_save(feed.source, NULL, snapshot);
      feed_cache_after = now + (saved ? CACHE_SAVE_INTERVAL_MS : CACHE_RETRY_INTERVAL_MS);
    }
    cJSON_Delete(snapshot);
    uint32_t delay;
    if (ok) {
      job->failures = 0;
      static const uint32_t intervals[SOURCE_COUNT] = {
        [SOURCE_STOCK] = STOCK_INTERVAL_MS, [SOURCE_NEWS] = NEWS_INTERVAL_MS,
        [SOURCE_FEED] = FEED_INTERVAL_MS,
      };
      delay = intervals[source];
    } else {
      if (job->failures < 6) job->failures++;
      delay = 5000U << job->failures;
      if (delay > RETRY_MAX_MS) delay = RETRY_MAX_MS;
    }
    job->due = now + delay;
    portENTER_CRITICAL(&lock);
    if (source != SOURCE_FEED || generation == visible_generation) atomic_store(&retrying[source], !ok);
    portEXIT_CRITICAL(&lock);
  }
}

/* Only the host-process lane starts the task. Other callers can queue before
 * creation, and both mailbox and task handle are published under the mux. */
static void observe(source_t next, const pet_wifi_data_feed_t *feed) {
  if (!pet_p4_is_wifi6_hardware()) return;
  static uint64_t retry_start;
  uint64_t now = now_ms();
  portENTER_CRITICAL(&lock);
  bool changed = next != visible || (feed && strcmp(feed->source, visible_feed.source));
  if (changed) {
    refresh_requested = false;
    needs_account = false;
    visible_generation++;
    atomic_store(&retrying[SOURCE_FEED], false);
    if (feed) visible_feed = *feed;
  }
  visible = next;
  TaskHandle_t task = worker;
  bool start = !task && (next != SOURCE_NONE || pending) && now >= retry_start;
  if (start) retry_start = now + 5000;
  portEXIT_CRITICAL(&lock);
  if (start) {
    TaskHandle_t created = NULL;
    if (xTaskCreate(wifi_data_worker, "pet_wifi_data", 8192, NULL, 2, &created) == pdPASS) {
      portENTER_CRITICAL(&lock);
      worker = task = created;
      portEXIT_CRITICAL(&lock);
    }
  }
  if (task && changed) xTaskNotifyGive(task);
}

void pet_wifi_data_observe(const char *source) {
  source_t next = source_id(source);
  observe(next == SOURCE_FEED ? SOURCE_NONE : next, NULL);
}

void pet_wifi_data_observe_feed(const pet_wifi_data_feed_t *feed) {
  bool configured = feed && feed->endpoint.host[0] && source_id(feed->source) == SOURCE_FEED;
  observe(configured ? SOURCE_FEED : SOURCE_NONE, configured ? feed : NULL);
}

void pet_wifi_data_refresh(const char *source) {
  source_t requested = source_id(source);
  portENTER_CRITICAL(&lock);
  TaskHandle_t task = worker;
  if (requested != SOURCE_NONE && requested == visible &&
      (requested != SOURCE_FEED || !strcmp(source, visible_feed.source))) refresh_requested = true;
  portEXIT_CRITICAL(&lock);
  if (task) xTaskNotifyGive(task);
}

bool pet_wifi_data_retrying(const char *source) {
  source_t requested = source_id(source);
  if (requested != SOURCE_FEED) return atomic_load(&retrying[requested]);
  portENTER_CRITICAL(&lock);
  bool failed = visible == SOURCE_FEED && !strcmp(source, visible_feed.source)
    && atomic_load(&retrying[SOURCE_FEED]);
  portEXIT_CRITICAL(&lock);
  return failed;
}

void pet_wifi_data_set_watchlist(const pet_wifi_watchlist_t *list, uint64_t revision) {
  portENTER_CRITICAL(&lock);
  bool newer = revision > pending_revision;
  if (newer) {
    pending_revision = revision;
    pending_list = *list;
    pending = true;
  }
  TaskHandle_t task = worker;
  portEXIT_CRITICAL(&lock);
  if (newer && task) xTaskNotifyGive(task);
}
#endif
