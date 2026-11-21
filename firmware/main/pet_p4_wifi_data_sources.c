#include "pet_p4_wifi_data_sources.h"

#if PET_P4_SYSTEM_WIFI
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pet_p4_json_source.h"

#define PET_WIFI_FEED_MAPPED_DEPTH_MAX 9
#define PET_WIFI_FEED_MAPPED_STRUCTURE_MAX 512

static bool digits(const char *s, size_t n) {
  if (!n) return false;
  for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

static bool symbol_valid(const char *s) {
  size_t len = strlen(s);
  if (len == 7 && !strncmp(s, "hk", 2)) return digits(s + 2, 5);
  if (len == 8 && (!strncmp(s, "sh", 2) || !strncmp(s, "sz", 2) || !strncmp(s, "bj", 2)))
    return digits(s + 2, 6);
  if (len < 3 || len > 14 || strncmp(s, "us", 2) || s[2] < 'A' || s[2] > 'Z') return false;
  for (size_t i = 3; i < len; i++)
    if (!((s[i] >= 'A' && s[i] <= 'Z') || digits(s + i, 1) || s[i] == '.' || s[i] == '-'))
      return false;
  return true;
}

bool pet_wifi_watchlist_equal(const pet_wifi_watchlist_t *a, const pet_wifi_watchlist_t *b) {
  if (a->count != b->count) return false;
  for (unsigned i = 0; i < a->count; i++) if (strcmp(a->symbols[i], b->symbols[i])) return false;
  return true;
}

bool pet_wifi_watchlist_parse(const cJSON *symbols, pet_wifi_watchlist_t *out) {
  if (!cJSON_IsArray(symbols) || cJSON_GetArraySize(symbols) > PET_P4_DATA_MAX_ROWS) return false;
  pet_wifi_watchlist_t list = {0};
  const cJSON *symbol;
  cJSON_ArrayForEach(symbol, symbols) {
    if (!cJSON_IsString(symbol) || !symbol->valuestring || !symbol_valid(symbol->valuestring))
      return false;
    for (unsigned i = 0; i < list.count; i++)
      if (!strcmp(symbol->valuestring, list.symbols[i])) return false;
    strcpy(list.symbols[list.count++], symbol->valuestring);
  }
  *out = list;
  return true;
}

bool pet_wifi_data_url(char out[PET_WIFI_DATA_URL_MAX], pet_wifi_data_endpoint_t endpoint,
                       const pet_wifi_watchlist_t *list, uint32_t item) {
  const char *hn = "https://hacker-news.firebaseio.com/v0";
  switch (endpoint) {
    case PET_WIFI_GET_HN_TOP:
      snprintf(out, PET_WIFI_DATA_URL_MAX, "%s/topstories.json?orderBy=%%22%%24key%%22&limitToFirst=8", hn);
      return true;
    case PET_WIFI_GET_HN_ITEM:
      if (!item) return false;
      snprintf(out, PET_WIFI_DATA_URL_MAX, "%s/item/%lu.json", hn, (unsigned long)item);
      return true;
    case PET_WIFI_GET_TENCENT_QUOTES: {
      if (!list || !list->count || list->count > PET_P4_DATA_MAX_ROWS) return false;
      strcpy(out, "https://qt.gtimg.cn/q=");
      for (unsigned i = 0; i < list->count; i++) {
        if (!memchr(list->symbols[i], 0, sizeof(list->symbols[i])) || !symbol_valid(list->symbols[i]))
          return false;
        if (i) strcat(out, ",");
        strcat(out, list->symbols[i]);
      }
      return true;
    }
  }
  return false;
}

static bool format_utc(char *out, size_t capacity, uint64_t ms, bool include_time) {
  time_t seconds = (time_t)(ms / 1000);
  struct tm tm;
  if (!gmtime_r(&seconds, &tm)) return false;
  int date_bytes = snprintf(out, capacity, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  if (date_bytes < 0 || (size_t)date_bytes >= capacity) return false;
  if (!include_time) return true;
  size_t remaining = capacity - (size_t)date_bytes;
  int time_bytes = snprintf(out + date_bytes, remaining, " %02d:%02d UTC", tm.tm_hour, tm.tm_min);
  return time_bytes >= 0 && (size_t)time_bytes < remaining;
}

cJSON *pet_wifi_data_snapshot(const char *source, const char *status, const char *message,
                            uint32_t ttl_ms, uint64_t utc_ms) {
  char date[11];
  cJSON *root = cJSON_CreateObject();
  if (!root || !format_utc(date, sizeof(date), utc_ms, false)
      || !cJSON_AddNumberToObject(root, "schema", 1)
      || !cJSON_AddStringToObject(root, "source", source)
      || !cJSON_AddStringToObject(root, "status", status)
      || !cJSON_AddStringToObject(root, "message", message)
      || !cJSON_AddStringToObject(root, "date", date)
      || !cJSON_AddNumberToObject(root, "ttlMs", ttl_ms)
      || !cJSON_AddArrayToObject(root, "rows")) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

static bool append_row(cJSON *root, const char *id, const char *label, const char *value,
                       const char *detail, const char *meta, int tone) {
  cJSON *row = cJSON_CreateObject();
  if (!row || !cJSON_AddStringToObject(row, "id", id)
      || !cJSON_AddStringToObject(row, "label", label)
      || !cJSON_AddStringToObject(row, "value", value)
      || !cJSON_AddStringToObject(row, "detail", detail)
      || !cJSON_AddStringToObject(row, "meta", meta)
      || !cJSON_AddNumberToObject(row, "tone", tone)
      || !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "rows"), row)) {
    cJSON_Delete(row);
    return false;
  }
  return true;
}

static bool decimal(const char *s, bool sign, size_t limit) {
  if (!s || !*s || strlen(s) > limit) return false;
  if (sign && (*s == '-' || *s == '+')) s++;
  const char *point = strchr(s, '.');
  return point ? digits(s, (size_t)(point - s)) && digits(point + 1, strlen(point + 1))
               : digits(s, strlen(s));
}

static bool quote_time(const char *input, char out[20]) {
  size_t n = strlen(input);
  if (n == 14 && digits(input, n)) {
    snprintf(out, 20, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", input, input + 4, input + 6,
             input + 8, input + 10, input + 12);
    return true;
  }
  if (n != 19) return false;
  for (size_t i = 0; i < n; i++) {
    if (i == 4 || i == 7) {
      if (input[i] != '-' && input[i] != '/') return false;
    } else if (i == 10 || i == 13 || i == 16) {
      if (input[i] != (i == 10 ? ' ' : ':')) return false;
    } else if (!digits(input + i, 1)) return false;
  }
  memcpy(out, input, 20);
  out[4] = out[7] = '-';
  return true;
}

typedef struct { const char *price, *change; char time[20]; } quote_fields_t;

cJSON *pet_wifi_stock_snapshot(char *body, const pet_wifi_watchlist_t *list, uint64_t utc_ms) {
  quote_fields_t quotes[PET_P4_DATA_MAX_ROWS] = {0};
  unsigned found = 0;
  char *rest = NULL;
  for (char *line = strtok_r(body, ";", &rest); line; line = strtok_r(NULL, ";", &rest)) {
    line += strspn(line, " \r\n\t");
    if (strncmp(line, "v_", 2)) continue;
    char *equal = strchr(line + 2, '=');
    if (!equal || equal[1] != '"') continue;
    *equal = 0;
    unsigned i = 0;
    while (i < list->count && strcmp(line + 2, list->symbols[i])) i++;
    if (i == list->count || quotes[i].price) continue;
    char *end = strrchr(equal + 2, '"');
    if (!end || end[1]) continue;
    *end = 0;
    char *fields[33], *field = equal + 2;
    unsigned n = 0;
    for (; n < 33 && field; n++) {
      fields[n] = field;
      field = strchr(field, '~');
      if (field) *field++ = 0;
    }
    if (n != 33 || !decimal(fields[3], false, 24)
        || (fields[32][0] && !decimal(fields[32], true, 22))
        || !quote_time(fields[30], quotes[i].time)) continue;
    quotes[i].price = fields[3];
    quotes[i].change = fields[32];
    found++;
  }
  if (list->count && !found) return NULL;
  cJSON *root = pet_wifi_data_snapshot(PET_WIFI_DATA_STOCKS, list->count ? "ok" : "empty",
      !list->count ? "请在 PC 组件中心添加自选股" : found < list->count ? "部分行情暂不可用" : "",
      30000, utc_ms + 8 * 3600000ULL);
  if (!root) return NULL;
  for (unsigned i = 0; i < list->count; i++) {
    const char *symbol = list->symbols[i];
    const quote_fields_t *q = &quotes[i];
    const char *currency = !strncmp(symbol, "hk", 2) ? "HKD" : !strncmp(symbol, "us", 2) ? "USD" : "CNY";
    char meta[64], detail[25] = "--";
    snprintf(meta, sizeof(meta), "%s %s", currency, q->price ? q->time : "等待有效行情");
    if (q->change && q->change[0]) snprintf(detail, sizeof(detail), "%s%s%%",
        q->change[0] == '-' || q->change[0] == '+' ? "" : "+", q->change);
    /* Tencent names are GBK. The stable ticker remains readable on the device. */
    if (!append_row(root, symbol, symbol, q->price ? q->price : "--", detail, meta, 0)) {
      cJSON_Delete(root);
      return NULL;
    }
  }
  return root;
}

static cJSON *parse_news(const char *body, size_t length) {
  if (!length || !pet_json_source_bounded_text_bytes(body, length)) return NULL;
  return cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
}

/* Check exact keys and duplicates before handing the typed values to the
 * widget validator. The response never supplies cache keys or policy fields. */
static bool feed_keys(const cJSON *object, const char *const *keys, unsigned count) {
  if (!cJSON_IsObject(object)) return false;
  unsigned seen = 0;
  const cJSON *field;
  cJSON_ArrayForEach(field, object) {
    unsigned i = 0;
    while (i < count && strcmp(field->string, keys[i])) i++;
    if (i == count || (seen & (1U << i))) return false;
    seen |= 1U << i;
  }
  return seen == (1U << count) - 1;
}

/* A byte cap alone still allows thousands of tiny cJSON nodes. Count
 * structural separators outside strings before allocating the JSON tree. */
static bool feed_shape(const char *body, size_t length, bool mapped) {
  if (!pet_json_source_text_limit_bytes(body, length, mapped ? PET_WIFI_FEED_MAPPED_DEPTH_MAX : 3)) return false;
  unsigned budget = mapped ? PET_WIFI_FEED_MAPPED_STRUCTURE_MAX : 7 * PET_P4_DATA_MAX_ROWS + 5;
  bool quoted = false;
  for (size_t i = 0; i < length; i++) {
    const char c = body[i];
    if (quoted && c == '\\') i++;
    else if (c == '"') quoted = !quoted;
    else if (!quoted && (c == ',' || c == '[' || c == '{') && !budget--) return false;
  }
  return true;
}

static const char *const feed_fields[] = {"rows", "id", "label", "value", "detail", "meta", "tone"};

static bool feed_path_valid(const char *path) {
  if (!*path || strlen(path) > PET_WIFI_FEED_PATH_MAX) return false;
  unsigned segments = 1;
  bool empty = true;
  for (const char *p = path; *p; p++) {
    const char c = *p;
    if (c == '.') {
      if (empty || ++segments > 4) return false;
      empty = true;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
      empty = false;
    }
  }
  return !empty;
}

bool pet_wifi_feed_map_parse(const cJSON *declaration, pet_wifi_feed_map_t *out) {
  memset(out, 0, sizeof(*out));
  if (!declaration) return true;
  if (!cJSON_IsObject(declaration)) return false;
  const cJSON *field;
  cJSON_ArrayForEach(field, declaration) {
    unsigned i = 0;
    while (i < PET_WIFI_FEED_FIELD_COUNT && strcmp(field->string, feed_fields[i])) i++;
    const char *path = cJSON_GetStringValue(field);
    if (i == PET_WIFI_FEED_FIELD_COUNT || out->paths[i][0] || !path
        || (!feed_path_valid(path) && (i != PET_WIFI_FEED_ROWS || strcmp(path, "$")))) return false;
    strcpy(out->paths[i], path);
  }
  return out->paths[PET_WIFI_FEED_ROWS][0] && out->paths[PET_WIFI_FEED_ID][0]
    && out->paths[PET_WIFI_FEED_LABEL][0];
}

/* Only exact object keys participate. A duplicate selected key is ambiguous. */
static const cJSON *feed_path_get(const cJSON *node, const char *path) {
  for (const char *segment = path; *segment;) {
    if (!cJSON_IsObject(node)) return NULL;
    size_t length = strcspn(segment, ".");
    const cJSON *match = NULL, *field;
    cJSON_ArrayForEach(field, node) {
      if (strncmp(field->string, segment, length) || field->string[length]) continue;
      if (match) return NULL;
      match = field;
    }
    node = match;
    segment += length;
    if (*segment == '.') segment++;
  }
  return node;
}

static const char *feed_mapped_text(const cJSON *field, bool id, char number[32], size_t capacity) {
  const char *value = cJSON_GetStringValue(field);
  if (cJSON_IsNumber(field) && isfinite(field->valuedouble)) {
    double n = field->valuedouble;
    if (id && (n < -9007199254740991.0 || n > 9007199254740991.0 || trunc(n) != n)) return NULL;
    snprintf(number, 32, id ? "%.0f" : "%.15g", n == 0 ? 0.0 : n);
    value = number;
  } else if (!id && cJSON_IsBool(field)) {
    value = cJSON_IsTrue(field) ? "true" : "false";
  }
  return value && strlen(value) < capacity ? value : NULL;
}

static bool feed_mapped_row(cJSON *snapshot, const cJSON *row, const pet_wifi_feed_map_t *map) {
  static const size_t capacities[] = {
    sizeof(((pet_p4_data_row_t *)0)->id), sizeof(((pet_p4_data_row_t *)0)->label),
    sizeof(((pet_p4_data_row_t *)0)->value), sizeof(((pet_p4_data_row_t *)0)->detail),
    sizeof(((pet_p4_data_row_t *)0)->meta),
  };
  if (!cJSON_IsObject(row)) return false;
  char numbers[5][32];
  const char *values[5];
  for (unsigned i = PET_WIFI_FEED_ID; i <= PET_WIFI_FEED_META; i++) {
    const char *path = map->paths[i];
    unsigned index = i - PET_WIFI_FEED_ID;
    values[index] = *path ? feed_mapped_text(feed_path_get(row, path), i == PET_WIFI_FEED_ID,
                                           numbers[index], capacities[index]) : "";
    if (!values[index]) return false;
  }
  int tone = 0;
  if (map->paths[PET_WIFI_FEED_TONE][0]) {
    const cJSON *field = feed_path_get(row, map->paths[PET_WIFI_FEED_TONE]);
    if (!cJSON_IsNumber(field) || field->valuedouble != field->valueint
        || field->valueint < -1 || field->valueint > 1) return false;
    tone = field->valueint;
  }
  return append_row(snapshot, values[0], values[1], values[2], values[3], values[4], tone);
}

static cJSON *feed_project(const cJSON *root, const pet_wifi_feed_map_t *map) {
  const char *path = map->paths[PET_WIFI_FEED_ROWS];
  const cJSON *rows = !strcmp(path, "$") ? root : feed_path_get(root, path);
  if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > PET_P4_DATA_MAX_ROWS) return NULL;
  cJSON *snapshot = cJSON_CreateObject();
  if (!snapshot || !cJSON_AddNumberToObject(snapshot, "schema", 1)
      || !cJSON_AddStringToObject(snapshot, "status", rows->child ? "ok" : "empty")
      || !cJSON_AddStringToObject(snapshot, "message", "")
      || !cJSON_AddArrayToObject(snapshot, "rows")) goto fail;
  const cJSON *row;
  cJSON_ArrayForEach(row, rows) {
    if (!feed_mapped_row(snapshot, row, map)) goto fail;
  }
  return snapshot;
fail:
  cJSON_Delete(snapshot);
  return NULL;
}

cJSON *pet_wifi_feed_snapshot(const char *source, const char *body, size_t length,
                             const pet_wifi_feed_map_t *map) {
  static const char *const root_keys[] = {"schema", "status", "message", "rows"};
  static const char *const row_keys[] = {"id", "label", "value", "detail", "meta", "tone"};
  bool mapped = map && map->paths[PET_WIFI_FEED_ROWS][0];
  if (!source || strncmp(source, PET_WIFI_DATA_FEED_PREFIX, sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1)
      || !length || length > PET_WIFI_DATA_BODY_MAX
      || !feed_shape(body, length, mapped)) return NULL;
  const char *end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts(body, length, &end, false);
  if (!root) return NULL;
  while (end < body + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
  if (end != body + length) goto fail;
  if (mapped) {
    cJSON *snapshot = feed_project(root, map);
    cJSON_Delete(root);
    root = snapshot;
    if (!root) return NULL;
  } else {
    if (!feed_keys(root, root_keys, 4)) goto fail;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "rows");
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > PET_P4_DATA_MAX_ROWS) goto fail;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
      if (!feed_keys(row, row_keys, 6)) goto fail;
    }
  }
  if (cJSON_AddStringToObject(root, "source", source)
      && cJSON_AddNumberToObject(root, "ttlMs", PET_WIFI_DATA_FEED_TTL_MS)
      && cJSON_AddBoolToObject(root, "clearOnExpire", true)) return root;
fail:
  cJSON_Delete(root);
  return NULL;
}

static bool json_uint(const cJSON *v, uint32_t *out) {
  if (!cJSON_IsNumber(v) || !(v->valuedouble >= 1 && v->valuedouble <= UINT32_MAX)) return false;
  uint32_t n = (uint32_t)v->valuedouble;
  if (n != v->valuedouble) return false;
  *out = n;
  return true;
}

bool pet_wifi_news_ids(const char *body, size_t length, uint32_t ids[8], unsigned *count) {
  cJSON *root = parse_news(body, length);
  bool valid = cJSON_IsArray(root) && cJSON_GetArraySize(root) <= 8;
  *count = 0;
  if (valid) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, root) {
      uint32_t id;
      if (!json_uint(entry, &id)) { valid = false; break; }
      unsigned i = 0;
      while (i < *count && ids[i] != id) i++;
      if (i == *count) ids[(*count)++] = id;
    }
  }
  cJSON_Delete(root);
  return valid;
}

static const char *headline_part(char *out, size_t capacity, const char *input) {
  size_t n = 0;
  while (input[n] && n + 1 < capacity) n++;
  while (n && ((unsigned char)input[n] & 0xc0) == 0x80) n--;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)input[i];
    out[i] = c < 32 || c == 127 ? ' ' : (char)c;
  }
  out[n] = 0;
  return input + n;
}

bool pet_wifi_news_add(cJSON *snapshot, const char *body, size_t length, uint32_t id, uint64_t utc_ms) {
  cJSON *item = parse_news(body, length);
  const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
  const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "title"));
  uint32_t got_id, seconds;
  bool ok = type && !strcmp(type, "story") && title && title[strspn(title, " \r\n\t")]
      && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "dead"))
      && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "deleted"))
      && json_uint(cJSON_GetObjectItemCaseSensitive(item, "id"), &got_id) && id == got_id
      && json_uint(cJSON_GetObjectItemCaseSensitive(item, "time"), &seconds)
      && seconds <= utc_ms / 1000 && utc_ms / 1000 - seconds <= 86400;
  if (ok) {
    char key[24], label[37], value[25], detail[25], meta[64];
    snprintf(key, sizeof(key), "%lu", (unsigned long)id);
    title = headline_part(label, sizeof(label), title);
    title = headline_part(value, sizeof(value), title);
    title = headline_part(detail, sizeof(detail) - 3, title);
    if (*title) strcat(detail, "...");
    ok = format_utc(meta, sizeof(meta), (uint64_t)seconds * 1000, true)
      && append_row(snapshot, key, label, value, detail, meta, 0);
  }
  cJSON_Delete(item);
  return ok;
}
#endif
