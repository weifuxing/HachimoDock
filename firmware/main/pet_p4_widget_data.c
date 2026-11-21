/* Validates full snapshots before atomic replacement; expires by monotonic time. */
#include "pet_p4_widget_data.h"
#if PET_P4_SYSTEM_WIFI
#include "pet_p4_wifi_data.h"
#include "esp_timer.h"
#include <stdio.h>
#endif
#if PET_P4_DATA_ROWS
#include "pet_p4_data_rows.h"
#endif
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
typedef struct {
  bool used, clear_on_expire, restored;
  char source[48], status[8], message[91], date[11];
  uint64_t received_ms;
  uint32_t ttl_ms;
  uint32_t usb_count, wifi_count;
  uint8_t count;
  pet_p4_data_row_t rows[PET_P4_DATA_MAX_ROWS];
} data_snapshot_t;
static EXT_RAM_BSS_ATTR data_snapshot_t g_sources[PET_P4_DATA_MAX_SOURCES];
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool snapshot_stale(const data_snapshot_t *s,uint64_t now_ms){
  return s->restored||now_ms<s->received_ms||now_ms-s->received_ms>s->ttl_ms;
}
#if PET_P4_SYSTEM_WIFI
static EXT_RAM_BSS_ATTR data_snapshot_t g_local_stock, g_local_news, g_local_feed;
static EXT_RAM_BSS_ATTR pet_wifi_watchlist_t g_watchlist;
static uint64_t g_watchlist_revision;
static bool feed_source(const char *source) {
  return !strcmp(source, PET_WIFI_DATA_FEED) ||
    !strncmp(source, PET_WIFI_DATA_FEED_PREFIX, sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1);
}
static data_snapshot_t *local_slot(const char *source) {
  if (!strcmp(source, PET_WIFI_DATA_STOCKS)) return &g_local_stock;
  if (!strcmp(source, PET_WIFI_DATA_NEWS)) return &g_local_news;
  if (strncmp(source, PET_WIFI_DATA_FEED_PREFIX, sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1) == 0)
    return &g_local_feed;
  return NULL;
}
#endif
/* Caller holds g_lock. A stale local quote yields to a fresh PC snapshot.
 * Keep the most recent cache if both sources have expired. */
static const data_snapshot_t *snapshot_for(const char *source, uint64_t now_ms) {
  const data_snapshot_t *pc = NULL;
  for (int i = 0; i < PET_P4_DATA_MAX_SOURCES; i++) {
    if (g_sources[i].used && !strcmp(g_sources[i].source, source)) {
      pc = &g_sources[i];
      break;
    }
  }
#if PET_P4_SYSTEM_WIFI
  const data_snapshot_t *local = local_slot(source);
  if (local && local->used && !strcmp(local->source, source) && (!pc || !snapshot_stale(local, now_ms) ||
      (snapshot_stale(pc, now_ms) &&
       (local->restored ? !pc->count : local->received_ms >= pc->received_ms)))) return local;
#else
  (void)now_ms;
#endif
  return pc;
}
#if PET_P4_DATA_ROWS
/* One active filter at a time (the visible component); values follow its int vars. */
static struct {bool used;char source[48];pet_data_rows_config_t config;int32_t values[PET_DATA_FILTERS_MAX];} g_filter;
/* Caller holds g_lock. Visible indexes after the active filter, source order. */
static unsigned visible_rows(const data_snapshot_t *s,uint8_t out[PET_P4_DATA_MAX_ROWS]){
  const pet_data_rows_config_t *c=g_filter.used&&!strcmp(g_filter.source,s->source)?&g_filter.config:NULL;
  return pet_data_rows_filter(s->rows,s->count,c,c?g_filter.values:NULL,out);
}
void pet_p4_widget_data_set_filter(const char *source,const pet_data_rows_config_t *config,const int32_t *values){
  portENTER_CRITICAL(&g_lock);
  if(!source||!source[0]||!config||!config->enabled){g_filter.used=false;}
  else{g_filter.used=true;strncpy(g_filter.source,source,sizeof(g_filter.source)-1);g_filter.source[sizeof(g_filter.source)-1]=0;g_filter.config=*config;
    for(unsigned i=0;i<PET_DATA_FILTERS_MAX;i++)g_filter.values[i]=values?values[i]:0;}
  portEXIT_CRITICAL(&g_lock);
}
bool pet_p4_widget_data_select(const char *source,int32_t select,uint64_t now_ms,pet_p4_data_row_t *out){
  bool ok=false;if(!source||!source[0]||!out)return false;
  portENTER_CRITICAL(&g_lock);
  const data_snapshot_t *s = snapshot_for(source, now_ms);
  if (s && !(s->clear_on_expire && snapshot_stale(s, now_ms))) {
    uint8_t visible[PET_P4_DATA_MAX_ROWS];unsigned n=visible_rows(s,visible);int at=pet_data_rows_select(n,select);
    if(at>=0){*out=s->rows[visible[at]];ok=true;}
  }
  portEXIT_CRITICAL(&g_lock);return ok;
}
#endif
static bool text(const cJSON *obj, const char *key, char *out, size_t size) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= size) return false;
  for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++) if (*p < 32 || *p == 127) return false;
  strcpy(out, v->valuestring);
  return true;
}
static bool integer(const cJSON *obj, const char *key, int min, int max, int *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max || v->valuedouble != v->valueint) return false;
  *out = v->valueint; return true;
}
static bool safe_source(const char *s) {
  if (!s[0]) return false;
  for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-')) return false;
  return true;
}
static bool apply(const cJSON *payload, uint64_t now_ms, bool local, uint64_t watchlist_revision,
                  bool restored, bool via_lan) {
  int schema, ttl;
  if (!cJSON_IsObject(payload) || !integer(payload, "schema", 1, 1, &schema)
      || !integer(payload, "ttlMs", 5000, 300000, &ttl)) return false;
  const cJSON *clear_on_expire = cJSON_GetObjectItemCaseSensitive(payload, "clearOnExpire");
  if (clear_on_expire && !cJSON_IsBool(clear_on_expire)) return false;
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(payload, "rows");
  if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > PET_P4_DATA_MAX_ROWS) return false;
  data_snapshot_t *next = calloc(1, sizeof(*next));
  if (!next) return false;
  bool ok = false;
  if (!text(payload, "source", next->source, sizeof(next->source)) || !safe_source(next->source)
      || !text(payload, "status", next->status, sizeof(next->status))
      || !text(payload, "message", next->message, sizeof(next->message))) goto done;
  if (strcmp(next->status, "ok") && strcmp(next->status, "error") && strcmp(next->status, "empty")) goto done;
  if (cJSON_HasObjectItem(payload, "date")) {
    if (!text(payload, "date", next->date, sizeof(next->date))) goto done;
    if (next->date[0]) {
      if (strlen(next->date) != 10) goto done;
      for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (next->date[i] != '-') goto done; }
        else if (next->date[i] < '0' || next->date[i] > '9') goto done;
      }
    }
  }
#if PET_P4_SYSTEM_WIFI
  const cJSON *settings = cJSON_GetObjectItemCaseSensitive(payload, "deviceWatchlist");
  pet_wifi_watchlist_t list;
  uint64_t settings_revision = 0;
  if ((restored && cJSON_IsTrue(clear_on_expire))
      || (local && !local_slot(next->source))
      || (!local && (!strcmp(next->source, PET_WIFI_DATA_NEWS) || feed_source(next->source)))
      || (settings && (local || strcmp(next->source, PET_WIFI_DATA_STOCKS)
          || !pet_wifi_watchlist_parse(settings, &list)))) goto done;
#else
  (void)local;
  (void)watchlist_revision;
#endif
  next->count = cJSON_GetArraySize(rows);
  for (int i = 0; i < next->count; i++) {
    const cJSON *row = cJSON_GetArrayItem(rows, i);
    pet_p4_data_row_t *r = &next->rows[i];
    int tone;
    if (!text(row, "id", r->id, sizeof(r->id)) || !r->id[0]
        || !text(row, "label", r->label, sizeof(r->label))
        || !text(row, "value", r->value, sizeof(r->value))
        || !text(row, "detail", r->detail, sizeof(r->detail))
        || !text(row, "meta", r->meta, sizeof(r->meta))
        || !integer(row, "tone", -1, 1, &tone)) goto done;
    for (int j = 0; j < i; j++) if (!strcmp(next->rows[j].id, r->id)) goto done;
    r->tone = tone;
  }
  next->used = true; next->ttl_ms = ttl; next->received_ms = now_ms;
  next->clear_on_expire = cJSON_IsTrue(clear_on_expire);
  next->restored = restored;
  portENTER_CRITICAL(&g_lock);
#if PET_P4_SYSTEM_WIFI
  if (local) {
    data_snapshot_t *slot = local_slot(next->source);
    if ((!restored || !slot->used || (slot == &g_local_feed && strcmp(slot->source, next->source))) &&
        (slot != &g_local_stock || watchlist_revision == g_watchlist_revision)) {
      *slot = *next;
      ok = true;
    }
  } else
#endif
  {
    int slot = -1;
    for (int i = 0; i < PET_P4_DATA_MAX_SOURCES; i++) if (g_sources[i].used && !strcmp(g_sources[i].source, next->source)) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < PET_P4_DATA_MAX_SOURCES; i++) if (!g_sources[i].used) { slot = i; break; }
    if (slot >= 0) {
      next->usb_count = g_sources[slot].usb_count;
      next->wifi_count = g_sources[slot].wifi_count;
      uint32_t *count = via_lan ? &next->wifi_count : &next->usb_count;
      if (*count < UINT32_MAX) ++*count;
      g_sources[slot] = *next;
      ok = true;
    }
  }
#if PET_P4_SYSTEM_WIFI
  if (ok && settings) {
    if (!g_watchlist_revision || !pet_wifi_watchlist_equal(&g_watchlist, &list)) {
      g_watchlist = list;
      g_watchlist_revision++;
      g_local_stock.used = false;
    }
    settings_revision = g_watchlist_revision;
  }
#endif
  portEXIT_CRITICAL(&g_lock);
#if PET_P4_SYSTEM_WIFI
  if (settings_revision) pet_wifi_data_set_watchlist(&list, settings_revision);
#endif
done:
  free(next); return ok;
}
bool pet_p4_widget_data_apply(const cJSON *payload, uint64_t now_ms) {
  return apply(payload, now_ms, false, 0, false, false);
}
bool pet_p4_widget_data_apply_lan(const cJSON *payload, uint64_t now_ms) {
  return apply(payload, now_ms, false, 0, false, true);
}

cJSON *pet_p4_widget_data_diagnostics(uint64_t now_ms) {
  struct {
    char source[48];
    bool fresh;
    uint32_t usb_count, wifi_count;
  } streams[PET_P4_DATA_MAX_SOURCES];
  size_t count = 0;
  portENTER_CRITICAL(&g_lock);
#if PET_P4_SYSTEM_WIFI
  int local_rows = g_local_feed.used && !snapshot_stale(&g_local_feed, now_ms)
    ? g_local_feed.count : -1;
#endif
  for (size_t i = 0; i < PET_P4_DATA_MAX_SOURCES; ++i) {
    const data_snapshot_t *s = &g_sources[i];
    if (!s->used) continue;
    strcpy(streams[count].source, s->source);
    streams[count].fresh = !snapshot_stale(s, now_ms);
    streams[count].usb_count = s->usb_count;
    streams[count++].wifi_count = s->wifi_count;
  }
  portEXIT_CRITICAL(&g_lock);

  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
#if PET_P4_SYSTEM_WIFI
  if (local_rows >= 0) {
    cJSON *feed = cJSON_AddObjectToObject(root, "device.feed");
    if (feed) cJSON_AddNumberToObject(feed, "freshRows", local_rows);
  }
#endif
  for (size_t i = 0; i < count; ++i) {
    cJSON *stream = cJSON_AddObjectToObject(root, streams[i].source);
    if (!stream || !cJSON_AddBoolToObject(stream, "received", true) ||
        !cJSON_AddBoolToObject(stream, "fresh", streams[i].fresh) ||
        !cJSON_AddNumberToObject(stream, "usbCount", streams[i].usb_count) ||
        !cJSON_AddNumberToObject(stream, "wifiCount", streams[i].wifi_count)) {
      cJSON_Delete(root);
      return NULL;
    }
  }
  return root;
}

#if PET_P4_SYSTEM_WIFI
bool pet_p4_widget_data_apply_local(const cJSON *payload, uint64_t now_ms, uint64_t watchlist_revision) {
  return apply(payload, now_ms, true, watchlist_revision, false, false);
}
bool pet_p4_widget_data_restore_local(const cJSON *payload, uint64_t now_ms, uint64_t watchlist_revision) {
  return apply(payload, now_ms, true, watchlist_revision, true, false);
}
void pet_p4_widget_data_clear_feed(const char *source) {
  portENTER_CRITICAL(&g_lock);
  if (g_local_feed.used && !strcmp(g_local_feed.source, source)) g_local_feed.used = false;
  portEXIT_CRITICAL(&g_lock);
}

bool pet_p4_widget_data_revalidate_feed(const char *source, uint64_t now_ms) {
  portENTER_CRITICAL(&g_lock);
  bool ok = g_local_feed.used && !g_local_feed.restored && !strcmp(g_local_feed.source, source);
  if (ok) g_local_feed.received_ms = now_ms;
  portEXIT_CRITICAL(&g_lock);
  return ok;
}
#endif
/* Caller holds g_lock. Preserve signed modulo for negative/int32-edge pages. */
static pet_p4_data_paging_t snapshot_paging(const data_snapshot_t *s,int32_t page){
  int count=s->count;
#if PET_P4_DATA_ROWS
  uint8_t visible[PET_P4_DATA_MAX_ROWS];count=(int)visible_rows(s,visible);
#endif
  int pages=count?(count+PET_P4_DATA_PAGE_ROWS-1)/PET_P4_DATA_PAGE_ROWS:1;
  return (pet_p4_data_paging_t){.pages=(uint8_t)pages,.page=(uint8_t)(((page%pages)+pages)%pages)};
}
pet_p4_data_paging_t pet_p4_widget_data_paging(const char *source,int32_t page){
  pet_p4_data_paging_t out={.pages=1};
  if(!source||!source[0])return out;
  uint64_t now_ms = 0;
#if PET_P4_SYSTEM_WIFI
  now_ms = (uint64_t)esp_timer_get_time() / 1000;
#endif
  portENTER_CRITICAL(&g_lock);
  const data_snapshot_t *s = snapshot_for(source, now_ms);
#if PET_P4_SYSTEM_WIFI
  if (s == &g_local_feed && s->clear_on_expire && snapshot_stale(s, now_ms)) s = NULL;
#endif
  if (s) out = snapshot_paging(s, page);
  portEXIT_CRITICAL(&g_lock);return out;
}
void pet_p4_widget_data_view(const char *source, int32_t page, uint64_t now_ms, pet_p4_data_view_t *out) {
  memset(out, 0, sizeof(*out));
  out->enabled = source && source[0]; out->pages = 1;
  if (!out->enabled) return;
#if PET_P4_SYSTEM_WIFI
  bool stock_local = false, feed_local = false;
#endif
  portENTER_CRITICAL(&g_lock);
  const data_snapshot_t *s = snapshot_for(source, now_ms);
  if (s) {
    out->received = true;
    out->stale = snapshot_stale(s,now_ms);
#if PET_P4_SYSTEM_WIFI
    out->restored = s->restored;
    out->age_ms = now_ms >= s->received_ms ? now_ms - s->received_ms : UINT64_MAX;
#endif
    if (s->clear_on_expire && out->stale) {
      strcpy(out->status, "empty");
      strcpy(out->message, "数据已过期，请刷新");
      portEXIT_CRITICAL(&g_lock);
      return;
    }
    pet_p4_data_paging_t paging=snapshot_paging(s,page);
    out->pages=paging.pages;out->page=paging.page;
    strcpy(out->status, s->status); strcpy(out->message, s->message);
    strcpy(out->date, s->date);
#if PET_P4_SYSTEM_WIFI
    stock_local = s == &g_local_stock;
    feed_local = s == &g_local_feed;
#endif
    int start = out->page * PET_P4_DATA_PAGE_ROWS;
#if PET_P4_DATA_ROWS
    uint8_t visible[PET_P4_DATA_MAX_ROWS];int total=(int)visible_rows(s,visible);
    out->count = total > start ? total - start : 0;
    if (out->count > PET_P4_DATA_PAGE_ROWS) out->count = PET_P4_DATA_PAGE_ROWS;
    for (int k = 0; k < out->count; k++) out->rows[k] = s->rows[visible[start + k]];
#else
    out->count = s->count > start ? s->count - start : 0;
    if (out->count > PET_P4_DATA_PAGE_ROWS) out->count = PET_P4_DATA_PAGE_ROWS;
    memcpy(out->rows, s->rows + start, out->count * sizeof(out->rows[0]));
#endif
  }
  portEXIT_CRITICAL(&g_lock);
#if PET_P4_SYSTEM_WIFI
  if ((stock_local || feed_local) && out->restored)
    snprintf(out->message, sizeof(out->message), "离线缓存（已过期）");
  else if (feed_local && out->stale)
    snprintf(out->message, sizeof(out->message), "设备缓存（已过期）");
  else if (stock_local && !out->message[0] && out->age_ms != UINT64_MAX)
    snprintf(out->message, sizeof(out->message), "设备直连 · 缓存 %llu 秒%s",
      (unsigned long long)(out->age_ms / 1000), out->stale ? "（已过期）" : "");
#endif
}
