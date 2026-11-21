#include "pet_p4_text_alpha.h"
/*
 * One route/client/boot-owned stage and host-owned asset scheduling. The cache
 * mutex protects metadata. A negotiated body can hold its previous immutable
 * mask briefly while the newest text remains authoritative for transfers.
 */
#include "pet_p4_bubble_pixels.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#define CACHE_SLOTS 8
#define CACHE_BYTES (256U * 1024U)
#define IMAGE_BYTES (32U * 1024U)
#define VIEW_SLOTS 12
#define IDLE_MS 3000U
#define ABSOLUTE_MS 10000U
#define SCHEMA "p4-text-alpha-v2:rle4:1:"
#define GLYPH_RECORD_BYTES (4U + PET_BUBBLE_GLYPH_BYTES)
#define GLYPH_MAX_BYTES (511U * GLYPH_RECORD_BYTES)
#define BODY_FIRST_WAIT_MS 2500U
#define BODY_UPDATE_WAIT_MS 3200U
#define VIEW_LEASE_MS 5000U
enum {PIXEL_PAGE, PIXEL_CELLS, PIXEL_BODY_MASK};
typedef struct {
  uint64_t        key;
  uint8_t        *data;
  size_t          size;
  uint32_t        checksum;
  unsigned        style, page, pages, age, readers;
  uint8_t         format;
  bool            retired;
} text_image_t;
typedef struct {
  uint64_t        key;
  unsigned        style;
} desired_asset_t;
/* One owner can advertise more than the original four assets: title, body,
 * compact body, compact title and the conversation status hints all belong to
 * the same owner. The old cap invalidated the whole manifest, so every host
 * frame was rejected as stale_content and bubbles stayed on local glyphs. */
#define DESIRED_ASSETS_PER_OWNER 12U
typedef struct {
  uint64_t        owner, version, scope;
  desired_asset_t assets[DESIRED_ASSETS_PER_OWNER];
  unsigned        asset_count;
} desired_t;
typedef struct {
  uint64_t        owner, target, since_ms, seen_ms, title_key, body_key;
  uint64_t        body_deadline, body_shown_key;
  unsigned        style;
  uint32_t        epoch;
  bool            occupied, title_bitmap, body_bitmap, body_waiting, body_local, title_waiting;
  uint8_t         body_page, body_shown_page;
} text_view_t;
static text_image_t cache[CACHE_SLOTS], staging;
static desired_t desired[VIEW_SLOTS];
static unsigned desired_count;
static text_view_t views[VIEW_SLOTS];
static size_t received, cache_bytes;
static unsigned stage_pixels, clock_age;
static uint32_t stage_hash;
static atomic_uint revision;
static uint64_t stage_owner, stage_version, stage_started, stage_seen, cache_now_ms;
static uint32_t lifecycle_epoch;
typedef struct {
  uint64_t        key;
  unsigned        style, page;
  uint8_t         format;
  /* The exact text the renderer is drawing. The host derives its own text set
   * per topic, and any drift there made the pixel key unmatchable, so the device
   * sends the content it actually needs rendered. */
  char            text[512];
  uint64_t        owner, version;
} need_t;
static need_t needs[8];
static unsigned need_count;
static uint64_t last_need_ms;
static char     boot[17], client[33], transfer[33];
static uintptr_t route;
static bool     blocked, conversation_hold, glyph_cells, body_masks, title_masks;
static SemaphoreHandle_t mutex;

static bool is_body(unsigned style) {
  return style == PET_BUBBLE_BODY || style == PET_BUBBLE_BODY_CLIPPED;
}

static uint8_t format_for(unsigned style) {
  if (title_masks && style == PET_BUBBLE_TITLE) return PIXEL_PAGE;
  return body_masks && is_body(style) ? PIXEL_BODY_MASK : glyph_cells ? PIXEL_CELLS : PIXEL_PAGE;
}

static bool owned_mask(unsigned style) {
  return (title_masks && style == PET_BUBBLE_TITLE) || (body_masks && is_body(style));
}

static bool paused_for(uint8_t format, unsigned style) {
  return blocked || (conversation_hold && format != PIXEL_BODY_MASK
    && !(title_masks && style == PET_BUBBLE_TITLE && format == PIXEL_PAGE));
}
static unsigned
width_for(unsigned s)
{
  return s == 1 ? 474 : (s == 2 || s == 4) ? 496 : 348;
}
static unsigned
height_for(unsigned s)
{
  return s == 2 || s == 4 ? 54 : 38;
}
static uint64_t hash_bytes(uint64_t hash, const char *s){
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  return hash;
}
/* Match the host's UTF-8 limits without changing the fallback text. */
static void copy_key_text(char *out, const char *text, size_t limit) {
  size_t len = 0;
  if (text) {
    while (len < limit && text[len]) len++;
    while (len > 0 && ((unsigned char) text[len] & 0xC0) == 0x80) len--;
    memcpy(out, text, len);
  }
  out[len] = 0;
}
static size_t body_key_limit(unsigned style) {
  return style == PET_BUBBLE_BODY_CLIPPED ? 383 : 511;
}

static uint64_t text_key(unsigned style, const char *text){
  uint64_t        h = hash_bytes(UINT64_C(14695981039346656037), SCHEMA);
  return hash_bytes((h ^ style) * UINT64_C(1099511628211), text);
}
/* Transfer parsing is sized separately from the framebuffer drawing path. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("Oz")
#endif
static const char *string_value(const cJSON * j, const char *n){
  const           cJSON *x = cJSON_GetObjectItemCaseSensitive(j, n);
  return cJSON_IsString(x) && x->valuestring ? x->valuestring : "";
}
static bool     number_value(const cJSON * j, const char *n, unsigned max, unsigned *out){
  const           cJSON *x = cJSON_GetObjectItemCaseSensitive(j, n);
  if (!cJSON_IsNumber(x) || !isfinite(x->valuedouble) || x->valuedouble < 0 || x->valuedouble > max || floor(x->valuedouble) != x->valuedouble)
    return false;
  *out = (unsigned)x->valuedouble;
  return true;
}
static bool
hex_string(const char *s, unsigned n)
{
  if (strlen(s) != n)
    return false;
  for (unsigned i = 0; i < n; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
      return false;
  return true;
}
static bool     hex_value(const char *s, unsigned n, uint64_t * out){
  if (!hex_string(s, n) || n > 16)
    return false;
  uint64_t        h = 0;
  for (unsigned i = 0; i < n; i++)
    h = (h << 4) | (unsigned)(s[i] <= '9' ? s[i] - '0' : s[i] - 'a' + 10);
  *out = h;
  return true;
}
static void     discard(text_image_t * i) {
  free(i->data);
  memset(i, 0, sizeof(*i));
}
static void     bump(void){
  atomic_fetch_add(&revision, 1);
}
static void     clear_stage(void){
  discard(&staging);
  received = stage_pixels = 0;
  transfer[0] = 0;
  stage_owner = stage_version = stage_started = stage_seen = 0;
}
static void     evict(int n){
  cache_bytes -= cache[n].size;
  discard(&cache[n]);
  bump();
}
static void     retire_cache(void){
  need_count = 0;
  last_need_ms = 0;
  lifecycle_epoch++;
  /* Exact masks remain reusable. The new epoch ends any previous-text hold. */
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && cache[i].format != PIXEL_BODY_MASK
        && !(title_masks && cache[i].style == PET_BUBBLE_TITLE && cache[i].format == PIXEL_PAGE)) {
      if (cache[i].readers)
        cache[i].retired = true;
      else
        evict(i);
    }
  bump();
}
static int
find(uint64_t key, unsigned style, unsigned page, uint8_t format)
{
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && !cache[i].retired && cache[i].key == key && cache[i].style == style
        && cache[i].page == page && cache[i].format == format)
      return i;
  return -1;
}
static bool
pinned(const text_image_t * image)
{
  if (image->readers)
    return true;
  for (unsigned i = 0; i < VIEW_SLOTS; i++) {
    const           text_view_t *v = &views[i];
    if (!v->occupied || cache_now_ms - v->seen_ms > VIEW_LEASE_MS
        || (image->format != PIXEL_BODY_MASK && v->epoch != lifecycle_epoch))
      continue;
    /* Keep the initial selection page and the requested page, not an entire
     * caption. Readers independently protect an in-flight immutable draw. */
    if ((v->title_bitmap && image->style == PET_BUBBLE_TITLE && image->key == v->title_key)
        || ((v->body_bitmap || image->format == PIXEL_BODY_MASK)
            && image->style == v->style && image->key == v->body_key
            && (image->page == 0 || image->page == v->body_page))
        || (image->format == PIXEL_BODY_MASK && image->style == v->style
            && image->key == v->body_shown_key && image->page == v->body_shown_page))
      return true;
  }
  return false;
}
static int
oldest(void)
{
  int             n = -1;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && !pinned(&cache[i]) && (n < 0 || cache[i].age < cache[n].age))
      n = i;
  return n;
}
static const desired_t *asset_owner(uint64_t key, unsigned style) {
  const desired_t *owned = NULL;
  for (unsigned i = 0; i < desired_count && !owned; i++)
    for (unsigned k = 0; k < desired[i].asset_count; k++)
      if (desired[i].assets[k].key == key && desired[i].assets[k].style == style) {
        owned = &desired[i];
        break;
      }
  return owned;
}
static bool
missing(uint64_t key, unsigned style, unsigned page, const char *text)
{
  const uint8_t format = format_for(style);
  if (!client[0])
    return false;
  const desired_t *owned = asset_owner(key, style);
  if (owned_mask(style) && !owned)
    return false;
  if (paused_for(format, style) || need_count >= sizeof(needs) / sizeof(needs[0]))
    return true;
  for (unsigned i = 0; i < need_count; i++)
    if (needs[i].key == key && needs[i].style == style && needs[i].page == page && needs[i].format == format)
      return true;
  need_t *slot = &needs[need_count++];
  *slot = (need_t) {0};
  slot->key = key;
  slot->style = style;
  slot->page = page;
  slot->format = format;
  /* owner/version let the host send a frame the device will still accept. */
  if (owned) {
    slot->owner = owned->owner;
    slot->version = owned->version;
  }
  snprintf(slot->text, sizeof(slot->text), "%s", text ? text : "");
  return true;
}
/* Throttle coalesced notices outside the renderer. */
static bool current_asset(uint64_t owner, uint64_t version, uint64_t key, unsigned style);

static bool
next_need(uint64_t now, pet_bubble_need_t * out, bool lan)
{
  if (!mutex || !out)
    return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (blocked || !client[0] || ((const void *)route == PET_BUBBLE_LAN_ROUTE) != lan
      || (last_need_ms && now - last_need_ms < (body_masks || title_masks ? 350 : 1000))) {
    xSemaphoreGive(mutex);
    return false;
  }
  while (need_count) {
    need_t          n = needs[0];
    memmove(needs, needs + 1, (--need_count) * sizeof(*needs));
    if (paused_for(n.format, n.style) || find(n.key, n.style, n.page, n.format) >= 0 || n.format != format_for(n.style)
        || (owned_mask(n.style) && !current_asset(n.owner, n.version, n.key, n.style)))
      continue;
    snprintf(out->id, sizeof(out->id), "%016" PRIx64, n.key);
    snprintf(out->owner, sizeof(out->owner), "%016" PRIx64, n.owner);
    snprintf(out->version, sizeof(out->version), "%016" PRIx64, n.version);
    snprintf(out->text, sizeof(out->text), "%s", n.text);
    snprintf(out->boot, sizeof(out->boot), "%s", boot);
    snprintf(out->client, sizeof(out->client), "%s", client);
    out->style = n.style;
    out->page = n.page;
    out->glyph_cells = n.format == PIXEL_CELLS;
    out->body_mask = n.format == PIXEL_BODY_MASK;
    out->route = (const void *)route;
    last_need_ms = now ? now : 1;
    xSemaphoreGive(mutex);
    return true;
  }
  xSemaphoreGive(mutex);
  return false;
}
static bool
current_asset(uint64_t owner, uint64_t version, uint64_t key, unsigned style)
{
  for (unsigned i = 0; i < desired_count; i++)
    if (desired[i].owner == owner && desired[i].version == version)
      for (unsigned k = 0; k < desired[i].asset_count; k++)
        if (desired[i].assets[k].key == key && desired[i].assets[k].style == style)
          return true;
  return false;
}
static void     expire(uint64_t now) {
  if (now > cache_now_ms) cache_now_ms = now;
  if (staging.data && (now < stage_started || now < stage_seen || now - stage_started >= ABSOLUTE_MS || now - stage_seen >= IDLE_MS || paused_for(staging.format, staging.style) || !current_asset(stage_owner, stage_version, staging.key, staging.style)))
    clear_stage();
}
bool pet_bubble_pixels_next_need(uint64_t now, pet_bubble_need_t *out) {
  return next_need(now, out, false);
}
bool pet_bubble_pixels_next_lan_need(uint64_t now, pet_bubble_need_t *out) {
  return next_need(now, out, true);
}
void pet_bubble_pixels_retire_route(const void *ctx) {
  if (!mutex) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (route == (uintptr_t)ctx) {
    clear_stage();
    retire_cache();
    client[0] = 0;
    title_masks = body_masks = glyph_cells = false;
    desired_count = need_count = 0;
  }
  xSemaphoreGive(mutex);
}
bool            pet_bubble_pixels_init(void){
  if (!mutex) {
    mutex = xSemaphoreCreateMutex();
    if (mutex) {
      uint64_t        b = ((uint64_t) esp_random() << 32) | esp_random();
      snprintf(boot, sizeof(boot), "%016" PRIx64, b ? b : 1);
    }
  } return mutex != NULL;
}
const char     *
pet_bubble_pixels_boot(void)
{
  return boot;
}
void            pet_bubble_pixels_maintain(uint64_t now, bool pause, bool conversation){
  if (!mutex)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  blocked = pause;
  conversation_hold = conversation;
  expire(now);
  xSemaphoreGive(mutex);
}
/*
 * Called only with accepted text state, before that state becomes visible. Payload tokens are
 * bounded capabilities for lifecycle correlation, not auth.
 */
void
pet_bubble_pixels_desire(const cJSON * payload, const void *ctx, const char *scope, uint64_t now)
{
  if (!mutex)
    return;
  const           cJSON *m = cJSON_GetObjectItemCaseSensitive(payload, "textPixels");
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (!cJSON_IsObject(m)) {
    if (client[0]) {
      clear_stage();
      retire_cache();
      client[0] = 0;
      title_masks = body_masks = glyph_cells = false;
      desired_count = 0;
    }
    xSemaphoreGive(mutex);
    return;
  }
  const char     *b = string_value(m, "boot"), *c = string_value(m, "client");
  const           cJSON *owners = cJSON_GetObjectItemCaseSensitive(m, "owners");
  int             n = cJSON_GetArraySize(owners);
  desired_t       next[VIEW_SLOTS];
  (void)scope; /* owner list is a complete native snapshot across display roles */
  bool            valid = !strcmp(b, boot) && hex_string(c, 32) && cJSON_IsArray(owners) && n >= 0 && n <= VIEW_SLOTS;
  for (int i = 0; valid && i < n; i++) {
    const           cJSON *v = cJSON_GetArrayItem(owners, i);
    valid = hex_value(string_value(v, "id"), 16, &next[i].owner) && hex_value(string_value(v, "version"), 16, &next[i].version);
    for (int k = 0; k < i; k++)
      if (next[k].owner == next[i].owner)
        valid = false;
    const           cJSON *assets = cJSON_GetObjectItemCaseSensitive(v, "assets");
    int             count = cJSON_GetArraySize(assets);
    if (!cJSON_IsArray(assets) || count < 0 || count > (int) DESIRED_ASSETS_PER_OWNER)
      valid = false;
    next[i].asset_count = valid ? (unsigned)count : 0;
    for (unsigned k = 0; valid && k < next[i].asset_count; k++) {
      const           cJSON *asset = cJSON_GetArrayItem(assets, (int)k);
      valid = hex_value(string_value(asset, "id"), 16, &next[i].assets[k].key) && number_value(asset, "style", 4, &next[i].assets[k].style) && next[i].assets[k].style > 0;
    }
  }
  if (valid) {
    bool cells = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "glyphCells"));
    bool masks = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "bodyMasks"));
    bool titles = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "titleMasks"));
    if (strcmp(c, client) || route != (uintptr_t) ctx || cells != glyph_cells || masks != body_masks || titles != title_masks) {
      clear_stage();
      retire_cache();
      desired_count = 0;
      snprintf(client, sizeof(client), "%s", c);
      route = (uintptr_t) ctx;
      glyph_cells = cells;
      body_masks = masks;
      title_masks = titles;
    }
    desired_count = (unsigned)n;
    memcpy(desired,next,desired_count*sizeof(*desired));
    unsigned keep = 0;
    for (unsigned i = 0; i < need_count; i++) {
      const need_t *need = &needs[i];
      if (!owned_mask(need->style) || current_asset(need->owner, need->version, need->key, need->style))
        needs[keep++] = *need;
    }
    need_count = keep;
    expire(now);
  }
  xSemaphoreGive(mutex);
}
static uint32_t glyph_codepoint(const uint8_t *record) {
  return (uint32_t)record[0] | (uint32_t)record[1] << 8 |
    (uint32_t)record[2] << 16 | (uint32_t)record[3] << 24;
}

static bool valid_cells(const uint8_t *data, size_t size) {
  uint32_t previous = 0;
  for (size_t offset = 0; offset < size; offset += GLYPH_RECORD_BYTES) {
    uint32_t cp = glyph_codepoint(data + offset);
    if (cp < 0x20 || cp <= previous || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    previous = cp;
  }
  return true;
}

static bool
handle_locked(const cJSON * p, const void *ctx, uint64_t now, cJSON * reply, const char **error)
{
  expire(now);
  const char     *phase = string_value(p, "phase");
  uint64_t        key, owner, version;
  if (strcmp(string_value(p, "boot"), boot) || strcmp(string_value(p, "client"), client) || route != (uintptr_t) ctx || !client[0]
      || !hex_value(string_value(p, "owner"), 16, &owner) || !hex_value(string_value(p, "version"), 16, &version)
  || !hex_value(string_value(p, "id"), 16, &key) || !hex_string(string_value(p, "transfer"), 32)) {
    *error = "stale_peer";
    return false;
  }
  bool            matching = staging.data && stage_owner == owner && stage_version == version && staging.key == key && !strcmp(transfer, string_value(p, "transfer"));
  if (!strcmp(phase, "abort")) {
    if (matching)
      clear_stage();
    return true;
  }
  unsigned        frame_style;
  if (!number_value(p, "style", 4, &frame_style) || !current_asset(owner, version, key, frame_style)) {
    if (matching)
      clear_stage();
    *error = "stale_content";
    return false;
  }
  if (!strcmp(phase, "status")) {
    cJSON_AddNumberToObject(reply, "offset", matching ? received : 0);
    cJSON_AddBoolToObject(reply, "receiving", matching);
    return true;
  }
  if (paused_for(format_for(frame_style), frame_style)) {
    if (matching)
      clear_stage();
    *error = "protected_audio_or_update";
    return false;
  }
  if (!strcmp(phase, "begin")) {
    if (staging.data) {
      *error = "stage_busy";
      return false;
    } unsigned      style, size, page, pages;
    uint64_t        sum;
    const           cJSON *sizes = cJSON_GetObjectItemCaseSensitive(p, "pageSizes");
    const           cJSON *only = cJSON_GetArrayItem(sizes, 0);
    if (!number_value(p, "style", 4, &style) || !style || !number_value(p, "totalBytes", IMAGE_BYTES, &size) || !size
        || !number_value(p, "pageIndex", 15, &page) || !number_value(p, "pageCount", 16, &pages) || !pages || page >= pages || (style != 2 && (page || pages != 1))
        || !hex_value(string_value(p, "checksum"), 8, &sum) || !cJSON_IsArray(sizes) || cJSON_GetArraySize(sizes) != 1 || !cJSON_IsNumber(only) || only->valuedouble != size
        || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "glyphCells")) != (format_for(style) == PIXEL_CELLS)
        || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "bodyMasks")) != (format_for(style) == PIXEL_BODY_MASK)
        || (format_for(style) == PIXEL_CELLS && (page || pages != 1 || size > GLYPH_MAX_BYTES || size % GLYPH_RECORD_BYTES))) {
      *error = "invalid_metadata";
      return false;
    }
    const uint8_t format = format_for(style);
    int             existing = find(key, style, page, format);
    if (existing >= 0) {
      text_image_t   *i = &cache[existing];
      if (i->size != size || i->checksum != (uint32_t) sum || i->pages != pages) {
        *error = "asset_conflict";
        return false;
      } cJSON_AddBoolToObject(reply, "cached", true);
      return true;
    }
    staging.data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!staging.data) {
      *error = "no_memory";
      return false;
    }
    staging.key = key;
    staging.size = size;
    staging.style = style;
    staging.page = page;
    staging.pages = pages;
    staging.format = format;
    staging.checksum = (uint32_t) sum;
    stage_owner = owner;
    stage_version = version;
    stage_started = stage_seen = now;
    stage_hash = 2166136261U;
    stage_pixels = received = 0;
    snprintf(transfer, sizeof(transfer), "%s", string_value(p, "transfer"));
    cJSON_AddNumberToObject(reply, "offset", 0);
    return true;
  }
  if (!matching) {
    *error = "no_matching_stage";
    return false;
  }
  if (!strcmp(phase, "chunk")) {
    unsigned        offset;
    const char     *data = string_value(p, "data");
    uint8_t         decoded[512];
    size_t          bytes = 0;
    if (!number_value(p, "offset", IMAGE_BYTES, &offset) || offset != received || !data[0] || strlen(data) > 684
        || mbedtls_base64_decode(decoded, sizeof(decoded), &bytes, (const unsigned char *)data, strlen(data)) || !bytes || bytes > staging.size - received) {
      clear_stage();
      *error = "invalid_chunk";
      return false;
    }
    unsigned        pixels = stage_pixels;
    uint32_t        hash = stage_hash;
    for (size_t i = 0; i < bytes; i++) {
      pixels += (decoded[i] & 15U) + 1U;
      hash = (hash ^ decoded[i]) * 16777619U;
    }
    if (staging.format != PIXEL_CELLS && pixels > width_for(staging.style) * height_for(staging.style)) {
      clear_stage();
      *error = "invalid_geometry";
      return false;
    }
    memcpy(staging.data + received, decoded, bytes);
    received += bytes;
    stage_pixels = pixels;
    stage_hash = hash;
    stage_seen = now;
    cJSON_AddNumberToObject(reply, "offset", received);
    return true;
  }
  if (strcmp(phase, "end") || received != staging.size || stage_hash != staging.checksum
      || (staging.format == PIXEL_CELLS ? !valid_cells(staging.data, staging.size)
                      : stage_pixels != width_for(staging.style) * height_for(staging.style))) {
    clear_stage();
    *error = "incomplete_or_invalid";
    return false;
  }
  while (cache_bytes + staging.size > CACHE_BYTES) {
    int             v = oldest();
    if (v < 0) {
      clear_stage();
      *error = "cache_pinned";
      return false;
    } evict(v);
  }
  int             slot = -1;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (!cache[i].data) {
      slot = i;
      break;
  }
  if (slot < 0)
    slot = oldest();
  if (slot < 0) {
    clear_stage();
    *error = "cache_pinned";
    return false;
  } if (cache[slot].data)
    evict(slot);
  staging.age = ++clock_age;
  cache[slot] = staging;
  cache_bytes += staging.size;
  memset(&staging, 0, sizeof(staging));
  received = 0;
  transfer[0] = 0;
  bump();
  return true;
}
bool
pet_bubble_pixels_handle_owned(const cJSON * p, const void *ctx, uint64_t now, cJSON * reply)
{
  if (!mutex || !reply)
    return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const char     *error = "invalid_request";
  bool            ok = handle_locked(p, ctx, now, reply, &error);
  cJSON_AddBoolToObject(reply, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(reply, "error", error);
  cJSON_AddStringToObject(reply, "boot", boot);
  cJSON_AddNumberToObject(reply, "cacheRevision", atomic_load(&revision));
  xSemaphoreGive(mutex);
  return ok;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
uint32_t        pet_bubble_pixels_revision(void){
  return atomic_load(&revision);
}
static void     unpin(int slot){
  if (slot >= 0 && slot < CACHE_SLOTS && cache[slot].readers) {
    cache[slot].readers--;
    if (!cache[slot].readers && cache[slot].retired)
      evict(slot);
  }
}
void            pet_bubble_pixels_release(pet_bubble_text_view_t * out) {
  if (!mutex || !out)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  unpin(out->title_pin);
  unpin(out->body_pin);
  out->title_pin = out->body_pin = -1;
  out->pixels = out->body_mask = out->body_exact = out->body_pending = false;
  xSemaphoreGive(mutex);
}
void
pet_bubble_pixels_pin_page(pet_bubble_text_view_t * out, unsigned page)
{
  if (!mutex || !out || (!out->body_mask && (!out->body_bitmap || out->glyph_cells)))
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (out->body_mask) {
    if (out->body_exact && out->epoch == lifecycle_epoch && page < out->body_pages && page != out->body_page) {
      const uint64_t key = cache[out->body_pin].key;
      int slot = find(key, out->style, page, PIXEL_BODY_MASK);
      if (slot >= 0) {
        unpin(out->body_pin);
        out->body_pin = slot;
        out->body_page = (uint8_t)page;
        cache[slot].readers++;
      } else if (body_masks) {
        missing(key, out->style, page, out->body);
      }
      for (unsigned i = 0; i < VIEW_SLOTS; i++) {
        text_view_t *v = &views[i];
        if (v->occupied && v->owner == out->owner_key && v->target == out->target_key
            && v->body_shown_key == key) {
          v->body_page = (uint8_t)page;
          v->body_shown_page = out->body_page;
        }
      }
    }
    xSemaphoreGive(mutex);
    return;
  }
  if (out->epoch != lifecycle_epoch) {
    out->pixels = false;
    xSemaphoreGive(mutex);
    return;
  } unpin(out->body_pin);
  out->body_pin = -1;
  char key_body[512];
  copy_key_text(key_body, out->body, body_key_limit(out->style));
  uint64_t key = text_key(out->style, key_body);
  /* Missing current pages also need a lease so their eventual upload cannot
   * be immediately displaced by unrelated prefetch. Never update a newer
   * view's lease through an older snapshot of the same owner. */
  if (page < 16) {
    for (unsigned i = 0; i < VIEW_SLOTS; i++) {
      text_view_t *v = &views[i];
      if (v->occupied && v->owner == out->owner_key && v->target == out->target_key)
        v->body_page = (uint8_t)page;
    }
  }
  int             slot = find(key, out->style, page, PIXEL_PAGE);
  if (slot >= 0) {
    out->body_pin = slot;
    cache[slot].readers++;
  } else if (out->body[0])
    missing(key, out->style, page, key_body);
  out->pixels = (!out->title_bitmap || out->title_pin >= 0) && out->body_pin >= 0;
  xSemaphoreGive(mutex);
}
static bool wants_asset(const char *text, unsigned style, unsigned (*coverage)(const char *, unsigned)) {
  if (!text[0]) return false;
  unsigned kinds = coverage ? coverage(text, style) : PET_BUBBLE_GLYPH_MISSING;
  /* Older senders supply a page without codepoint positions. That page can
   * display a fully unsupported string, but cannot replace a resident run. */
  return (kinds & PET_BUBBLE_GLYPH_MISSING) && (glyph_cells || !(kinds & PET_BUBBLE_GLYPH_RESIDENT));
}

/* One deadline for an unresolved stream of partials. Local fallback is scoped
 * to the current body and never prevents promotion to its exact mask. */
static int select_body_mask(text_view_t *v, pet_bubble_text_view_t *out,
    const char *text, uint64_t key, uint64_t now, bool retain_previous) {
  if (!text[0]) {
    v->body_waiting = v->body_local = false;
    v->body_deadline = v->body_shown_key = 0;
    return -1;
  }
  int previous = v->body_shown_key
    ? find(v->body_shown_key, out->style, v->body_shown_page, PIXEL_BODY_MASK) : -1;
  int exact = previous >= 0 && v->body_shown_key == key
    ? previous : find(key, out->style, 0, PIXEL_BODY_MASK);
  if (exact >= 0) {
    if (v->body_shown_key != key) v->since_ms = now;
    v->body_shown_key = key;
    v->body_shown_page = (uint8_t)cache[exact].page;
    v->body_waiting = v->body_local = false;
    v->body_deadline = 0;
    out->body_exact = true;
    return exact;
  }
  if (!retain_previous) {
    v->body_shown_key = 0;
    previous = -1;
  }
  if (body_masks && client[0]) {
    bool owned = missing(key, out->style, 0, text);
    /* The authenticated RTC HUD and its manifest arrive on different lanes.
     * A caption may become visible just before the matching owner is installed. */
    if ((owned || out->style == PET_BUBBLE_BODY) && !v->body_local) {
      if (!v->body_deadline)
        v->body_deadline = now + (previous >= 0 ? BODY_UPDATE_WAIT_MS : BODY_FIRST_WAIT_MS);
      if (now < v->body_deadline) {
        v->body_waiting = true;
        out->body_pending = previous < 0;
        return previous;
      }
    }
  }
  const bool was_waiting = v->body_waiting;
  v->body_waiting = false;
  v->body_local = true;
  v->body_shown_key = 0;
  if (was_waiting) bump(); /* A cached Agent overlay must repaint at the deadline. */
  return -1;
}

void
pet_bubble_pixels_select(const char *owner, unsigned style, const char *title, const char *body,
    uint64_t now, pet_bubble_text_view_t * out, unsigned (*coverage)(const char *, unsigned))
{
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  out->title_pin = out->body_pin = -1;
  out->style = style;
  snprintf(out->title, sizeof(out->title), "%s", title ? title : "");
  copy_key_text(out->body, body, sizeof(out->body) - 1);
  char key_title[192], key_body[512];
  copy_key_text(key_title, title, style == PET_BUBBLE_BODY ? 63 : 191);
  copy_key_text(key_body, body, body_key_limit(style));
  if (!mutex)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (now > cache_now_ms) cache_now_ms = now;
  out->epoch = lifecycle_epoch;
  out->glyph_cells = glyph_cells;
  uint64_t        tk = text_key(1, key_title), bk = text_key(style, key_body), id = text_key(style, owner ? owner : "");
  text_view_t    *v = NULL, *victim = &views[0];
  for (unsigned i = 0; i < VIEW_SLOTS; i++) {
    if (views[i].occupied && views[i].owner == id) {
      v = &views[i];
      break;
    } if (!views[i].occupied || views[i].seen_ms < victim->seen_ms)
      victim = &views[i];
  }
  if (!v) {
    v = victim;
    memset(v, 0, sizeof(*v));
    v->owner = id;
    v->occupied = true;
  }
  if (now < v->seen_ms) now = v->seen_ms;
  const bool expired_view = now - v->seen_ms > VIEW_LEASE_MS;
  const bool title_changed = v->title_key != tk;
  const bool peer_changed = v->epoch != lifecycle_epoch;
  if (expired_view || title_changed) {
    v->body_waiting = v->body_local = false;
    v->body_deadline = 0;
  }
  uint64_t        target = tk ^ (bk * UINT64_C(1099511628211));
  if (v->target != target) {
    /* A new update after a quiet period is independent of the previous
     * fallback. Fast partials still share the original bounded deadline. */
    if (v->body_local && now - v->since_ms >= BODY_FIRST_WAIT_MS)
      v->body_deadline = 0;
    v->body_local = false;
    v->target = target;
    v->since_ms = now;
    v->body_page = 0;
  }
  if (title_changed || peer_changed || title_masks)
    v->title_bitmap = title_masks ? key_title[0] && asset_owner(tk, PET_BUBBLE_TITLE)
      : wants_asset(key_title, PET_BUBBLE_TITLE, coverage);
  if (v->body_key != bk || peer_changed)
    v->body_bitmap = wants_asset(key_body, style, coverage);
  v->seen_ms = now;
  v->title_key = tk;
  v->body_key = bk;
  v->style = style;
  v->epoch = lifecycle_epoch;
  out->since_ms = v->since_ms;
  out->owner_key = id;
  out->target_key = target;
  out->title_bitmap = v->title_bitmap;
  out->body_bitmap = v->body_bitmap;
  const bool retain_previous = !expired_view && !title_changed && !peer_changed;
  int b = is_body(style) ? select_body_mask(v, out, key_body, bk, now, retain_previous) : -1;
  out->since_ms = v->since_ms;
  if (b >= 0) {
    out->body_mask = true;
    out->body_page = (uint8_t)cache[b].page;
    out->body_pages = (uint8_t)cache[b].pages;
  } else if (out->body_bitmap && !(body_masks && is_body(style))) {
    b = find(bk, style, 0, format_for(style));
  }
  if (out->body_bitmap && b < 0 && !(body_masks && is_body(style)))
    missing(bk, style, 0, key_body);
  int a = out->title_bitmap ? find(tk, PET_BUBBLE_TITLE, 0, format_for(PET_BUBBLE_TITLE)) : -1;
  bool title_owned = out->title_bitmap && a < 0
    && missing(tk, PET_BUBBLE_TITLE, 0, key_title);
  /* An owned host title has one presentation: never paint a thin local title
   * just because delivery is slow. No timer or retained previous-title text.
   * Unmanaged/offline titles immediately keep the local renderer instead. */
  out->title_pending = title_masks && key_title[0] && a < 0 && title_owned;
  if (v->title_waiting != out->title_pending) bump();
  v->title_waiting = out->title_pending;
  out->title_mask = title_masks && a >= 0;
  if (a >= 0) {
    out->title_pin = a;
    cache[a].readers++;
  } if (b >= 0) {
    out->body_pin = b;
    cache[b].readers++;
  }
  out->pixels = !out->glyph_cells && (out->title_bitmap || out->body_bitmap)
    && (title_masks || !out->title_bitmap || a >= 0) && (!out->body_bitmap || b >= 0);
  xSemaphoreGive(mutex);
}
unsigned        pet_bubble_pixels_pages(unsigned style, const char *text){
  if (!mutex || !text || !*text)
    return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  char key_body[512];
  copy_key_text(key_body, text, body_key_limit(style));
  uint64_t        key = text_key(style, key_body);
  unsigned        pages = 0;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && !cache[i].retired && cache[i].key == key && cache[i].style == style
        && cache[i].format == PIXEL_PAGE) {
      pages = cache[i].pages;
      break;
    }
  xSemaphoreGive(mutex);
  return pages;
}

void pet_text_alpha_blit(const uint8_t *data, size_t size, unsigned width,
    uint16_t *framebuffer, int x, int y, int clip_width, uint16_t color) {
  unsigned pixel = 0;
  for (size_t i = 0; i < size; i++) {
    unsigned        alpha = data[i] >> 4, run = (data[i] & 15U) + 1U;
    if (!alpha) { pixel += run; continue; }
    for (unsigned end = pixel + run; pixel < end; pixel++) {
      int             col = (int)(pixel % width), dx = x + col, dy = y + (int)(pixel / width);
      if (col >= clip_width || dx < 0 || dx >= 640 || dy < 0 || dy >= 480)
        continue;
      uint16_t       *out = framebuffer + dy * 640 + dx;
      *out = pet_text_alpha_blend565(*out, color, alpha);
    }
  }
}

bool
pet_bubble_pixels_draw_pinned(const pet_bubble_text_view_t * view, unsigned style, unsigned page, uint16_t * framebuffer, int x, int y, int clip_width, uint16_t color)
{
  if (!mutex || !view || !framebuffer || (!view->pixels && !(is_body(style) && view->body_mask)
      && !(style == PET_BUBBLE_TITLE && view->title_mask)))
    return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  int             slot = style == PET_BUBBLE_TITLE ? view->title_pin : view->body_pin;
  if (slot < 0 || slot >= CACHE_SLOTS || !cache[slot].data || !cache[slot].readers || cache[slot].style != style || cache[slot].page != page) {
    xSemaphoreGive(mutex);
    return false;
  }
  text_image_t   *image = &cache[slot];
  image->age = ++clock_age;
  /* A draw owns a separate pin: a caller may release its view or the peer
   * may retire it after this unlock. Only the final unpin may free the page. */
  image->readers++;
  const uint8_t *data = image->data;
  size_t size = image->size;
  unsigned        width = width_for(style);
  xSemaphoreGive(mutex);
  pet_text_alpha_blit(data, size, width, framebuffer, x, y, clip_width, color);
  xSemaphoreTake(mutex, portMAX_DELAY);
  unpin(slot);
  xSemaphoreGive(mutex);
  return true;
}

static bool copy_cell(const text_image_t *image, uint32_t codepoint, uint8_t *out) {
  size_t lo = 0, hi = image->size / GLYPH_RECORD_BYTES;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const uint8_t *record = image->data + mid * GLYPH_RECORD_BYTES;
    uint32_t cp = glyph_codepoint(record);
    if (cp == codepoint) {
      memcpy(out, record + 4, PET_BUBBLE_GLYPH_BYTES);
      return true;
    }
    if (cp < codepoint) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

bool pet_bubble_pixels_copy_glyph(const pet_bubble_text_view_t *view, unsigned style,
    uint32_t codepoint, uint8_t out[PET_BUBBLE_GLYPH_BYTES]) {
  if (!mutex || !view || !view->glyph_cells || !out) return false;
  int slot = style == PET_BUBBLE_TITLE ? view->title_pin : view->body_pin;
  /* A view pin keeps that asset readable through peer retirement. */
  if (slot >= 0 && slot < CACHE_SLOTS && cache[slot].format == PIXEL_CELLS
      && copy_cell(&cache[slot], codepoint, out)) return true;
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool found = false;
  if (view->epoch == lifecycle_epoch && glyph_cells) {
    /* Each cell is keyed by its scalar from an admitted asset of this same
     * renderer. Appending text must not unpaint an already delivered glyph. */
    for (int i = 0; i < CACHE_SLOTS && !found; i++) {
      if (i != slot && cache[i].data && !cache[i].retired && cache[i].format == PIXEL_CELLS
          && copy_cell(&cache[i], codepoint, out)) {
        cache[i].age = ++clock_age;
        found = true;
      }
    }
  }
  xSemaphoreGive(mutex);
  return found;
}
