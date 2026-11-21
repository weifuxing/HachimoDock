/* Track C component media store; see pet_p4_component_media.h. */
#include "pet_p4_component_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include "pet_p4_file_cleanup.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if PET_P4_WASM_AUDIO
#include "mbedtls/sha256.h"
#endif
#ifdef ESP_PLATFORM
#include "freertos/task.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_fs_activity.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#endif
static const char *TAG = "p4-cmedia";
typedef struct { pet_cext_media_kind_t kind; char id[PET_CEXT_ID_MAX]; uint32_t offset, size, next_index, last_offset, last_size; bool complete; } cmedia_file_t;
#define CMEDIA_FILES (PET_SAMPLE_MAX_SLOTS + PET_CEXT_CLIP_MAX + PET_CEXT_BIG_MAX)
static struct { uint8_t *arena; uint32_t used; cmedia_file_t files[CMEDIA_FILES]; unsigned count; } g_stage;
#if PET_P4_WASM_AUDIO
static struct { uint8_t *bytes; size_t size, used; } g_speech_stage;
#endif
typedef struct {
  uint8_t *samples; uint32_t sample_offset[PET_SAMPLE_MAX_SLOTS], sample_frames[PET_SAMPLE_MAX_SLOTS]; unsigned sample_count;
  uint8_t *clip; size_t clip_bytes; pet_cext_clip_t clip_meta; bool clip_ready;
  char big_path[PET_CEXT_BIG_MAX][32]; pet_cext_big_t big_meta[PET_CEXT_BIG_MAX]; uint8_t *big_table[PET_CEXT_BIG_MAX]; unsigned big_count;
#if PET_P4_WASM_AUDIO
  uint8_t *speech;
  size_t speech_size;
#endif
  uint32_t generation;
  unsigned pins;
} cmedia_bank_t;
static cmedia_bank_t g_banks[PET_CMEDIA_BANKS];
static unsigned g_current;
static uint32_t g_generation;
static bool g_generation_exhausted;
static struct { uint64_t serial; uint32_t generation; unsigned bank; bool empty; } g_pending;
static uint64_t g_prepare_serial;
#define g_active g_banks[g_current]
static struct { _Atomic(uint32_t) token; unsigned bank; } g_leases[PET_CMEDIA_LEASES];
static uint32_t g_token, g_reclaimed, g_deferred_reclaimed;
#define CMEDIA_RELEASED (UINT32_C(1) << 31)
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(unsigned) == 4, "audio release requires lock-free 32-bit atomics");
static void reap_locked(void);
static _Atomic(SemaphoreHandle_t) g_lock;
static void lock(void) {
  SemaphoreHandle_t mutex = atomic_load(&g_lock);
  if (!mutex) {
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) abort();
    SemaphoreHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong(&g_lock, &expected, candidate)) {
      vSemaphoreDelete(candidate); mutex = expected;
    } else mutex = candidate;
  }
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) abort();
  reap_locked();
}
/* Frame/main probes never allocate a mutex or collect/free implicitly. */
static bool try_lock(void) {
  SemaphoreHandle_t mutex = atomic_load(&g_lock);
  return mutex && xSemaphoreTake(mutex, 0) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(atomic_load(&g_lock)); }
static char kind_char(pet_cext_media_kind_t k) { return k == PET_CEXT_MEDIA_SAMPLE ? 's' : k == PET_CEXT_MEDIA_CLIP ? 'c' : 'b'; }
static void media_path(size_t slot, uint8_t generation, pet_cext_media_kind_t kind, unsigned index, bool temporary, char *out, size_t size) {
  snprintf(out, size, temporary ? "/spiffs/p4m%02u-%u-%c%u.tmp" : "/spiffs/p4m%02u-%u-%c%u.bin", (unsigned) slot, (unsigned) generation, kind_char(kind), index);
}
static uint32_t kind_budget(pet_cext_media_kind_t kind) {
  return kind == PET_CEXT_MEDIA_SAMPLE ? PET_SAMPLE_MAX_BYTES + PET_CEXT_SAMPLE_HEADER : kind == PET_CEXT_MEDIA_CLIP ? PET_CEXT_CLIP_MAX_BYTES : PET_CEXT_BIG_BUDGET_BYTES;
}
static void fail(char *e, size_t n, const char *m) { if (e && n) snprintf(e, n, "%s", m); }

void pet_cmedia_stage_reset(void) {
  free(g_stage.arena);
  memset(&g_stage, 0, sizeof(g_stage));
#if PET_P4_WASM_AUDIO
  free(g_speech_stage.bytes);
  memset(&g_speech_stage, 0, sizeof(g_speech_stage));
#endif
}

#if PET_P4_WASM_AUDIO
static void speech_path(size_t slot, uint8_t generation, bool temporary, char path[32]) {
  snprintf(path, 32, "/spiffs/p4m%02u-%u-a0.%s", (unsigned)slot, (unsigned)generation,
           temporary ? "tmp" : "bin");
}

static bool speech_digest_matches(const uint8_t hash[32], const char *expected) {
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i)
    if (expected[i * 2] != hex[hash[i] >> 4] || expected[i * 2 + 1] != hex[hash[i] & 15])
      return false;
  return true;
}

/* Yield between VFS calls so a bank read or write cannot monopolize the
 * protocol worker or hold the flash cache disabled across scheduler ticks. */
static void speech_io_yield(void) {
#ifdef ESP_PLATFORM
  vTaskDelay(1);
#endif
}

static bool speech_matches(const uint8_t *bytes, size_t size, const char *expected) {
  uint8_t hash[32];
  if (!pet_cext_speech_bank_valid(bytes, size)) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
  for (size_t offset = 0; ok && offset < size;) {
    const size_t remaining = size - offset;
    const size_t length = remaining > 4096 ? 4096 : remaining;
    ok = mbedtls_sha256_update(&ctx, bytes + offset, length) == 0;
    offset += length;
    speech_io_yield();
  }
  ok = ok && mbedtls_sha256_finish(&ctx, hash) == 0 && speech_digest_matches(hash, expected);
  mbedtls_sha256_free(&ctx);
  return ok;
}

static bool speech_write(const char *tmp, const char *path, const uint8_t *bytes, size_t size) {
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = setvbuf(f, NULL, _IONBF, 0) == 0;
  for (size_t offset = 0; ok && offset < size;) {
    size_t length = size - offset;
    if (length > 4096) length = 4096;
    ok = fwrite(bytes + offset, 1, length, f) == length;
    offset += length;
    speech_io_yield();
  }
  if (fclose(f) != 0) ok = false;
  if (ok) ok = rename(tmp, path) == 0;
  if (!ok) unlink(tmp);
  return ok;
}

static uint8_t *speech_read(const char *path, size_t *size) {
  struct stat info;
  if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      (uint64_t)info.st_size > PET_CEXT_SPEECH_MAX_BYTES) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  *size = (size_t)info.st_size;
  uint8_t *bytes = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bool ok = bytes != NULL;
  for (size_t offset = 0; ok && offset < *size;) {
    size_t length = *size - offset;
    if (length > 4096) length = 4096;
    ok = fread(bytes + offset, 1, length, f) == length;
    offset += length;
    speech_io_yield();
  }
  ok = ok && fgetc(f) == EOF && !ferror(f);
  if (fclose(f) != 0) ok = false;
  if (ok) return bytes;
  free(bytes);
  return NULL;
}

bool pet_cmedia_speech_stage(size_t file_size, size_t offset, const uint8_t *bytes,
                             size_t len, char *error, size_t error_size) {
  if (!bytes || !len || file_size < PET_CEXT_SPEECH_HEADER + 8u + PET_CEXT_SPEECH_BLOCK_BYTES ||
      file_size > PET_CEXT_SPEECH_MAX_BYTES || offset != g_speech_stage.used ||
      (g_speech_stage.size && g_speech_stage.size != file_size) ||
      offset > file_size || len > file_size - offset) {
    fail(error, error_size, "invalid WASM speech sidecar length or offset");
    return false;
  }
  if (!g_speech_stage.bytes) {
    g_speech_stage.bytes = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_speech_stage.bytes) {
      fail(error, error_size, "WASM speech staging memory unavailable");
      return false;
    }
    g_speech_stage.size = file_size;
  }
  memcpy(g_speech_stage.bytes + offset, bytes, len);
  g_speech_stage.used += len;
  return true;
}

/* Catalog recovery verifies the selected generation before exposing its entry.
 * Stream the file so a catalog scan does not allocate a second bank. */
bool pet_cmedia_speech_verify_file(size_t slot, uint8_t generation, const char *expected) {
  if (!expected || strlen(expected) != 64 || slot >= PET_CEV_COMPONENTS || generation > 1)
    return false;
  char path[32];
  speech_path(slot, generation, false, path);
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint8_t buffer[2048], hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
  size_t total = 0, length;
  while (ok && (length = fread(buffer, 1, sizeof(buffer), f)) != 0) {
    total += length;
    ok = total <= PET_CEXT_SPEECH_MAX_BYTES &&
         mbedtls_sha256_update(&ctx, buffer, length) == 0;
    speech_io_yield();
  }
  ok = ok && feof(f) && !ferror(f) && total >= PET_CEXT_SPEECH_HEADER + 8u + PET_CEXT_SPEECH_BLOCK_BYTES
      && mbedtls_sha256_finish(&ctx, hash) == 0 && speech_digest_matches(hash, expected);
  mbedtls_sha256_free(&ctx);
  return fclose(f) == 0 && ok;
}
#endif
bool pet_cmedia_stage_chunk(pet_cext_media_kind_t kind, const char *id, uint32_t index, const uint8_t *data, size_t len, char *error, size_t size) {
  if (kind < PET_CEXT_MEDIA_SAMPLE || kind > PET_CEXT_MEDIA_BIG || !pet_cev_name_valid(id) || !data || !len || len > kind_budget(kind) || index == UINT32_MAX) { fail(error, size, "invalid component media chunk"); return false; }
  if (!g_stage.arena) g_stage.arena = heap_caps_malloc(PET_CMEDIA_STAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_stage.arena) { fail(error, size, "component media staging memory unavailable"); return false; }
  cmedia_file_t *f = NULL;
  for (unsigned i = 0; i < g_stage.count; i++) if (g_stage.files[i].kind == kind && !strcmp(g_stage.files[i].id, id)) f = &g_stage.files[i];
  if (f && f->next_index && index == f->next_index - 1) {
    bool same = len == f->last_size && !memcmp(g_stage.arena + f->last_offset, data, len);
    if (!same) fail(error, size, "component media retransmission differs");
    return same;
  }
  if (f && (f->complete || f->offset + f->size != g_stage.used)) {
    fail(error, size, "component media files must not interleave"); return false;
  }
  if (!f) {
    if (g_stage.count && !g_stage.files[g_stage.count - 1].complete) { fail(error, size, "previous component media file is incomplete"); return false; }
    if (index != 0 || g_stage.count >= CMEDIA_FILES) { fail(error, size, "component media chunk sequence"); return false; }
    f = &g_stage.files[g_stage.count++]; memset(f, 0, sizeof(*f)); f->kind = kind; strcpy(f->id, id); f->offset = g_stage.used;
  }
  if (index != f->next_index) { fail(error, size, "component media chunk out of sequence"); return false; }
  uint32_t kind_total = f->size + (uint32_t) len;
  for (unsigned i = 0; i < g_stage.count; i++) if (&g_stage.files[i] != f && g_stage.files[i].kind == kind && kind != PET_CEXT_MEDIA_CLIP && kind != PET_CEXT_MEDIA_SAMPLE) kind_total += g_stage.files[i].size;
  if (kind_total > kind_budget(kind) || g_stage.used + len > PET_CMEDIA_STAGE_BYTES) { fail(error, size, "component media exceeds its budget"); return false; }
  if (kind == PET_CEXT_MEDIA_SAMPLE) {
    uint32_t total = len; for (unsigned i = 0; i < g_stage.count; i++) if (g_stage.files[i].kind == kind) total += g_stage.files[i].size;
    if (total > PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER) { fail(error, size, "sample pack exceeds 256 KiB"); return false; }
  }
  f->last_offset = g_stage.used; f->last_size = (uint32_t) len;
  memcpy(g_stage.arena + g_stage.used, data, len);
  g_stage.used += (uint32_t) len; f->size += (uint32_t) len; f->next_index = index + 1;
  /* Headers carry the exact size, so completion needs no end-of-file message. */
  const uint8_t *d = g_stage.arena + f->offset;
  if (kind == PET_CEXT_MEDIA_SAMPLE && f->size >= PET_CEXT_SAMPLE_HEADER && !memcmp(d, "P4A1", 4)) {
    uint32_t frames = (uint32_t) d[4] | (uint32_t) d[5] << 8 | (uint32_t) d[6] << 16 | (uint32_t) d[7] << 24;
    f->complete = frames && frames <= PET_SAMPLE_MAX_BYTES / 2 && f->size == frames * 2 + PET_CEXT_SAMPLE_HEADER;
  } else if (kind == PET_CEXT_MEDIA_CLIP && f->size >= PET_CEXT_CLIP_HEADER && !memcmp(d, "P4V1", 4)) {
    uint32_t bytes = (uint32_t) d[12] | (uint32_t) d[13] << 8 | (uint32_t) d[14] << 16 | (uint32_t) d[15] << 24;
    f->complete = bytes && bytes <= PET_CEXT_CLIP_MAX_BYTES - PET_CEXT_CLIP_HEADER && f->size == bytes + PET_CEXT_CLIP_HEADER;
  } else if (kind == PET_CEXT_MEDIA_BIG && f->size >= PET_CEXT_BIG_HEADER && !memcmp(d, "P4T1", 4)) {
    unsigned count = d[14]; uint32_t end = 0;
    if (f->size >= PET_CEXT_BIG_HEADER + count * 8) for (unsigned i = 0; i < count; i++) { uint32_t o, b; if (pet_cext_big_tile(d, f->size, i, &o, &b) && o + b > end) end = o + b; }
    f->complete = count && count <= PET_CEXT_BIG_MAX_TILES && end && f->size == end;
  }
  return true;
}
static const cmedia_file_t *staged(pet_cext_media_kind_t kind, const char *id) {
  for (unsigned i = 0; i < g_stage.count; i++) if (g_stage.files[i].kind == kind && !strcmp(g_stage.files[i].id, id) && g_stage.files[i].complete) return &g_stage.files[i];
  return NULL;
}
bool pet_cmedia_stage_matches(const pet_cext_runtime_t *r, char *error, size_t size) {
  if (!r) return g_stage.count == 0
#if PET_P4_WASM_AUDIO
      && !g_speech_stage.size
#endif
      ;
  if (r->sample_count > PET_SAMPLE_MAX_SLOTS || r->clip_count > PET_CEXT_CLIP_MAX || r->big_count > PET_CEXT_BIG_MAX) return false;
  for (unsigned i = 0; i < r->sample_count; i++) {
    const cmedia_file_t *f = staged(PET_CEXT_MEDIA_SAMPLE, r->sample[i].id);
    if (!f || !pet_cext_sample_header(g_stage.arena + f->offset, f->size, NULL)) { fail(error, size, "declared sample is missing or malformed"); return false; }
  }
  for (unsigned i = 0; i < r->clip_count; i++) {
    const cmedia_file_t *f = staged(PET_CEXT_MEDIA_CLIP, r->clips[i].id);
    if (!f || !pet_cext_clip_header(g_stage.arena + f->offset, f->size, NULL)) { fail(error, size, "declared clip is missing or malformed"); return false; }
  }
  for (unsigned i = 0; i < r->big_count; i++) {
    const cmedia_file_t *f = staged(PET_CEXT_MEDIA_BIG, r->bigs[i].id); pet_cext_big_t meta;
    if (!f || !pet_cext_big_header(g_stage.arena + f->offset, f->size, &meta) || meta.w != r->bigs[i].w || meta.h != r->bigs[i].h) { fail(error, size, "declared big image is missing or malformed"); return false; }
  }
#if PET_P4_WASM_AUDIO
  const bool declared = r->speech_sha256[0] != 0;
  if (declared != (g_speech_stage.size != 0) ||
      (declared && (g_speech_stage.used != g_speech_stage.size ||
          !speech_matches(g_speech_stage.bytes, g_speech_stage.size, r->speech_sha256)))) {
    fail(error, size, "WASM speech sidecar is missing, malformed, or differs from SHA-256");
    return false;
  }
#endif
  if (g_stage.count != (unsigned)r->sample_count + r->clip_count + r->big_count) { fail(error, size, "undeclared component media file"); return false; }
  return true;
}
static bool write_atomic(const char *tmp, const char *final, const uint8_t *data, size_t len) {
  FILE *f = fopen(tmp, "wb"); if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len; ok = fclose(f) == 0 && ok;
  if (ok) ok = rename(tmp, final) == 0; /* failed rename preserves the previous file */
  if (!ok) unlink(tmp);
  return ok;
}
/* Count actual files in the reserved namespace. A failed or interrupted
 * directory read is never accepted as proof of unused media space. */
enum { CMEDIA_BUDGET_SCAN_US = 5000000 };
static bool budget_scan_current(uint64_t epoch, int64_t deadline, unsigned step) {
#ifdef ESP_PLATFORM
  if (step && step % 16 == 0) vTaskDelay(1);
  uint64_t observed;
  return esp_timer_get_time() < deadline && pet_fs_activity_idle_epoch(&observed)
    && observed == epoch;
#else
  (void)epoch; (void)deadline; (void)step;
  return true;
#endif
}

static bool media_budget_name(const char *name) {
  // p4m00-0-s0.bin; count both published and spare/temporary generations.
  if (strlen(name) != 14 || memcmp(name, "p4m", 3)
      || name[3] < '0' || name[3] > '9' || name[4] < '0' || name[4] > '9'
      || name[5] != '-' || (name[6] != '0' && name[6] != '1') || name[7] != '-'
      || name[9] < '0' || name[9] > '9'
      || (strcmp(name + 10, ".bin") && strcmp(name + 10, ".tmp"))) return false;
  const unsigned slot = (unsigned)(name[3] - '0') * 10 + (unsigned)(name[4] - '0');
  const unsigned index = (unsigned)(name[9] - '0');
  return slot < PET_CEV_COMPONENTS && ((name[8] == 's' && index < PET_SAMPLE_MAX_SLOTS)
    || (name[8] == 'c' && index < PET_CEXT_CLIP_MAX)
    || (name[8] == 'b' && index < PET_CEXT_BIG_MAX)
#if PET_P4_WASM_AUDIO
    || (name[8] == 'a' && index == 0)
#endif
  );
}

static bool disk_budget_ok(void) {
  size_t total = g_stage.used;
#if PET_P4_WASM_AUDIO
  total += g_speech_stage.used;
#endif
  if (total > PET_CMEDIA_DEVICE_BUDGET_BYTES) return false;
  if (!total) return true;

  uint64_t epoch = 0;
  int64_t deadline = 0;
#ifdef ESP_PLATFORM
  deadline = esp_timer_get_time() + CMEDIA_BUDGET_SCAN_US;
  // Allocated SPIFFS payload pages bound all existing media file bytes.
  // Most devices can prove the budget with this O(1) allocation counter.
  size_t used = SIZE_MAX;
  if (esp_spiffs_info("storage", NULL, &used) == ESP_OK
      && used <= PET_CMEDIA_DEVICE_BUDGET_BYTES - total) return true;
  if (!pet_fs_activity_idle_epoch(&epoch)) return false;
#endif
  DIR *directory = opendir("/spiffs");
  if (!directory) return false;

  /* Finish enumeration before stat: an ESP-IDF SPIFFS lookup can leave a
   * shared errno behind, which older VFS versions can report at directory EOF. */
  enum { BUDGET_FILES = CMEDIA_FILES + (PET_P4_WASM_AUDIO ? 1 : 0),
         MEDIA_NAMES = PET_CEV_COMPONENTS * 4 * BUDGET_FILES };
  uint8_t present[(MEDIA_NAMES + 7) / 8] = {0};
  bool complete = false;
  unsigned step = 0;
  for (; step <= PET_FILE_CLEANUP_MAX_ENTRIES && budget_scan_current(epoch, deadline, step); ++step) {
    errno = 0;
    const struct dirent *entry = readdir(directory);
    if (!entry) { complete = errno == 0; break; }
    if (!memchr(entry->d_name, 0, sizeof(entry->d_name))) break;
    const char *name = entry->d_name;
    if (!media_budget_name(name)) continue;
    const unsigned slot = (unsigned)(name[3] - '0') * 10 + (unsigned)(name[4] - '0');
    const unsigned generation = (unsigned)(name[6] - '0');
    const unsigned type_base = name[8] == 'a' ? CMEDIA_FILES : name[8] == 's' ? 0 : PET_SAMPLE_MAX_SLOTS
      + (name[8] == 'c' ? 0 : PET_CEXT_CLIP_MAX);
    const unsigned item = type_base + (unsigned)(name[9] - '0');
    const unsigned bit = ((slot * 2 + generation) * BUDGET_FILES + item) * 2
      + (name[11] == 't');
    const uint8_t mask = (uint8_t)(1u << (bit % 8));
    if (present[bit / 8] & mask) break;
    present[bit / 8] |= mask;
  }
  if (closedir(directory) != 0 || !complete || !budget_scan_current(epoch, deadline, 0)) return false;

  char path[32];
  for (unsigned bit = 0; bit < MEDIA_NAMES; ++bit) {
    if (!(present[bit / 8] & (1u << (bit % 8)))) continue;
    if (!budget_scan_current(epoch, deadline, ++step)) return false;
    const unsigned pair = bit / 2;
    unsigned index = pair % BUDGET_FILES;
#if PET_P4_WASM_AUDIO
    if (index == CMEDIA_FILES) {
      speech_path(pair / BUDGET_FILES / 2, (pair / BUDGET_FILES) % 2, bit % 2 != 0, path);
    } else
#endif
    {
    pet_cext_media_kind_t kind = PET_CEXT_MEDIA_SAMPLE;
    if (index >= PET_SAMPLE_MAX_SLOTS) {
      index -= PET_SAMPLE_MAX_SLOTS;
      kind = PET_CEXT_MEDIA_CLIP;
      if (index >= PET_CEXT_CLIP_MAX) {
        index -= PET_CEXT_CLIP_MAX;
        kind = PET_CEXT_MEDIA_BIG;
      }
    }
    media_path(pair / BUDGET_FILES / 2, (pair / BUDGET_FILES) % 2, kind, index,
      bit % 2 != 0, path, sizeof(path));
    }
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0
        || (uint64_t)info.st_size > PET_CMEDIA_DEVICE_BUDGET_BYTES - total) return false;
    total += (size_t)info.st_size;
  }
  return budget_scan_current(epoch, deadline, 0);
}
static bool persist_locked(size_t slot, uint8_t generation, const pet_cext_runtime_t *r);
bool pet_cmedia_persist(size_t slot, uint8_t generation, const pet_cext_runtime_t *r) {
  if (slot >= PET_CEV_COMPONENTS || generation > 1 || !pet_cmedia_stage_matches(r, NULL, 0)) return false;
  lock();
  bool ok = !g_pending.serial && disk_budget_ok();
  if (ok) ok = persist_locked(slot, generation, r);
  else ESP_LOGW(TAG, "component media budget unavailable or exceeded");
  unlock();
  return ok;
}
static bool persist_locked(size_t slot, uint8_t generation, const pet_cext_runtime_t *r) {
  if (!r) return true;
  if (!pet_cmedia_stage_matches(r, NULL, 0)) return false;
  char path[32], tmp[32];
  for (pet_cext_media_kind_t kind = PET_CEXT_MEDIA_SAMPLE; kind <= PET_CEXT_MEDIA_BIG; kind++) {
    unsigned count = kind == PET_CEXT_MEDIA_SAMPLE ? r->sample_count : kind == PET_CEXT_MEDIA_CLIP ? r->clip_count : r->big_count;
    unsigned max = kind == PET_CEXT_MEDIA_SAMPLE ? PET_SAMPLE_MAX_SLOTS : kind == PET_CEXT_MEDIA_CLIP ? PET_CEXT_CLIP_MAX : PET_CEXT_BIG_MAX;
    for (unsigned i = 0; i < max; i++) {
      media_path(slot, generation, kind, i, false, path, sizeof(path)); media_path(slot, generation, kind, i, true, tmp, sizeof(tmp));
      if (i >= count) { unlink(path); unlink(tmp); continue; }
      const char *id = kind == PET_CEXT_MEDIA_SAMPLE ? r->sample[i].id : kind == PET_CEXT_MEDIA_CLIP ? r->clips[i].id : r->bigs[i].id;
      const cmedia_file_t *f = staged(kind, id);
      if (!f || !write_atomic(tmp, path, g_stage.arena + f->offset, f->size)) { ESP_LOGW(TAG, "component media persist failed %s", path); return false; }
    }
  }
#if PET_P4_WASM_AUDIO
  speech_path(slot, generation, false, path);
  speech_path(slot, generation, true, tmp);
  /* The storage owner selected the spare generation. SPIFFS rename refuses
   * existing names, so remove only the inactive sidecar before replacing it. */
  if (unlink(path) != 0 && errno != ENOENT) return false;
  if (r->speech_sha256[0]) {
    if (!speech_write(tmp, path, g_speech_stage.bytes, g_speech_stage.used)) return false;
  } else {
    unlink(tmp);
  }
#endif
  return true;
}
void pet_cmedia_remove(size_t slot) {
  if (slot >= PET_CEV_COMPONENTS) return;
  lock();
  if (g_pending.serial) { unlock(); return; }
  char path[32];
  for (uint8_t generation = 0; generation < 2; generation++)
    for (pet_cext_media_kind_t kind = PET_CEXT_MEDIA_SAMPLE; kind <= PET_CEXT_MEDIA_BIG; kind++)
      for (unsigned i = 0; i < PET_SAMPLE_MAX_SLOTS; i++) {
        media_path(slot, generation, kind, i, false, path, sizeof(path)); unlink(path);
        media_path(slot, generation, kind, i, true, path, sizeof(path)); unlink(path);
      }
#if PET_P4_WASM_AUDIO
  for (uint8_t generation = 0; generation < 2; ++generation) {
    speech_path(slot, generation, false, path);
    unlink(path);
    speech_path(slot, generation, true, path);
    unlink(path);
  }
#endif
  unlock();
}
static uint8_t *read_file(const char *path, size_t max, size_t *out_size) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  uint8_t *buffer = heap_caps_malloc(max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t n = buffer ? fread(buffer, 1, max, f) : 0; int extra = buffer ? fgetc(f) : EOF; bool io_error = ferror(f); fclose(f);
  if (!buffer || !n || extra != EOF || io_error) { free(buffer); return NULL; }
  *out_size = n; return buffer;
}
static void bank_free(cmedia_bank_t *bank) {
  free(bank->samples); free(bank->clip);
#if PET_P4_WASM_AUDIO
  free(bank->speech);
#endif
  for (unsigned i = 0; i < PET_CEXT_BIG_MAX; i++) free(bank->big_table[i]);
  memset(bank, 0, sizeof(*bank));
}
static bool bank_visible(const cmedia_bank_t *bank) {
  return bank->sample_count || bank->clip_ready || bank->big_count
#if PET_P4_WASM_AUDIO
      || bank->speech_size
#endif
      ;
}

static void bank_hide(cmedia_bank_t *bank) {
  bank->sample_count = 0;
  bank->clip_ready = false;
  bank->big_count = 0;
#if PET_P4_WASM_AUDIO
  bank->speech_size = 0;
#endif
}

static void reap_locked(void) {
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) {
    if (!(atomic_load(&g_leases[i].token) & CMEDIA_RELEASED)) continue;
    unsigned bank = g_leases[i].bank;
    g_reclaimed++; g_deferred_reclaimed++;
    if (--g_banks[bank].pins == 0 && (bank != g_current || !bank_visible(&g_active))) bank_free(&g_banks[bank]);
    atomic_store(&g_leases[i].token, 0);
  }
}
void pet_cmedia_collect(void) { lock(); unlock(); }
bool pet_cmedia_collect_try(void) {
  if (!try_lock()) return false;
  reap_locked(); unlock(); return true;
}
bool pet_cmedia_generation_try(uint32_t expected) {
  if (!expected || !try_lock()) return false;
  bool ok = expected == g_generation; unlock(); return ok;
}
bool pet_cmedia_unpin_deferred(uint64_t token) {
  if (!token || token >= CMEDIA_RELEASED) return false;
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) {
    uint32_t expected = (uint32_t)token;
    if (atomic_compare_exchange_strong(&g_leases[i].token, &expected, (uint32_t)token | CMEDIA_RELEASED)) return true;
  }
  return false;
}
void pet_cmedia_unload(void) {
  lock();
  if (g_pending.serial) { unlock(); return; }
  /* Detach visibility immediately; pinned buffers retire until their last unpin. */
  if (!g_active.pins) bank_free(&g_active);
  bank_hide(&g_active);
  if (g_generation == UINT32_MAX) { g_generation = 0; g_generation_exhausted = true; }
  else if (!g_generation_exhausted) g_generation++;
  unlock();
}
bool pet_cmedia_load(size_t slot, uint8_t generation, const pet_cext_runtime_t *r) {
  pet_cmedia_ticket_t ticket = {0};
  return pet_cmedia_prepare(slot, generation, r, &ticket) && pet_cmedia_commit(&ticket);
}
bool pet_cmedia_prepare(size_t slot, uint8_t generation, const pet_cext_runtime_t *r, pet_cmedia_ticket_t *ticket) {
  if (!ticket || ticket->serial || slot >= PET_CEV_COMPONENTS || generation > 1) return false;
  bool ok = true; char path[32];
  lock();
  if (g_pending.serial || g_prepare_serial == UINT64_MAX || g_generation_exhausted || g_generation == UINT32_MAX) { unlock(); return false; }
  if (r && (r->sample_count > PET_SAMPLE_MAX_SLOTS || r->clip_count > PET_CEXT_CLIP_MAX || r->big_count > PET_CEXT_BIG_MAX)) { unlock(); return false; }
  if (!r || (!r->sample_count && !r->clip_count && !r->big_count
#if PET_P4_WASM_AUDIO
      && !r->speech_sha256[0]
#endif
      )) {
    g_pending.serial = ++g_prepare_serial; g_pending.generation = g_generation; g_pending.empty = true;
    *ticket = (pet_cmedia_ticket_t){g_pending.serial, g_generation}; unlock(); return true;
  }
  unsigned previous = g_current, target = PET_CMEDIA_BANKS;
  for (unsigned i = 0; i < PET_CMEDIA_BANKS; i++) if (i != previous && !g_banks[i].pins) { target = i; break; }
  if (target == PET_CMEDIA_BANKS) { unlock(); return false; }
  bank_free(&g_banks[target]); g_current = target;
  if (r->sample_count) {
    g_active.samples = heap_caps_malloc(PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t used = 0;
    for (unsigned i = 0; ok && g_active.samples && i < r->sample_count; i++) {
      media_path(slot, generation, PET_CEXT_MEDIA_SAMPLE, i, false, path, sizeof(path));
      size_t n = 0; uint8_t *data = read_file(path, PET_SAMPLE_MAX_BYTES + PET_CEXT_SAMPLE_HEADER, &n); pet_cext_sample_t meta;
      ok = data && pet_cext_sample_header(data, n, &meta) && used + n <= PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER;
      if (ok) { memcpy(g_active.samples + used, data, n); g_active.sample_offset[i] = (uint32_t) used + PET_CEXT_SAMPLE_HEADER; g_active.sample_frames[i] = meta.frames; used += n; g_active.sample_count++; }
      free(data);
    }
    ok = ok && g_active.samples;
  }
  if (ok && r->clip_count) {
    media_path(slot, generation, PET_CEXT_MEDIA_CLIP, 0, false, path, sizeof(path));
    size_t n = 0; g_active.clip = read_file(path, PET_CEXT_CLIP_MAX_BYTES, &n);
    ok = g_active.clip && pet_cext_clip_header(g_active.clip, n, &g_active.clip_meta);
    if (ok) { strcpy(g_active.clip_meta.id, r->clips[0].id); g_active.clip_bytes = n; g_active.clip_ready = true; }
  }
  size_t big_total = 0;
  for (unsigned i = 0; ok && i < r->big_count; i++) {
    media_path(slot, generation, PET_CEXT_MEDIA_BIG, i, false, g_active.big_path[i], sizeof(g_active.big_path[i]));
    FILE *f = fopen(g_active.big_path[i], "rb"); uint8_t header[PET_CEXT_BIG_HEADER + PET_CEXT_BIG_MAX_TILES * 8];
    size_t n = f ? fread(header, 1, sizeof(header), f) : 0; long total = 0;
    if (f) { if (!ferror(f) && fseek(f, 0, SEEK_END) == 0) total = ftell(f); fclose(f); }
    ok = n >= PET_CEXT_BIG_HEADER && total > 0 && (size_t) total <= PET_CEXT_BIG_BUDGET_BYTES;
    if (ok) { big_total += (size_t)total; ok = big_total <= PET_CEXT_BIG_BUDGET_BYTES; }
    if (ok) {
      /* Validate the table against the file length without loading tiles. */
      uint8_t *table = heap_caps_malloc(sizeof(header), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      ok = table != NULL; if (ok) { memcpy(table, header, n); g_active.big_table[i] = table; }
      unsigned count = header[14];
      ok = ok && count && count <= PET_CEXT_BIG_MAX_TILES && n >= PET_CEXT_BIG_HEADER + count * 8 && !memcmp(header, "P4T1", 4);
      unsigned tw = header[6] | (unsigned) header[7] << 8, th = header[8] | (unsigned) header[9] << 8;
      unsigned w = header[10] | (unsigned) header[11] << 8, h = header[12] | (unsigned) header[13] << 8;
      ok = ok && header[4] && header[5] && header[4] * header[5] == count && tw && th && tw <= PET_CEXT_BIG_TILE && th <= PET_CEXT_BIG_TILE
        && w == r->bigs[i].w && h == r->bigs[i].h && tw * header[4] >= w && th * header[5] >= h
        && tw * (header[4]-1) < w && th * (header[5]-1) < h;
      uint32_t end = PET_CEXT_BIG_HEADER + count * 8;
      for (unsigned t = 0; ok && t < count; t++) {
        uint32_t o, b;
        ok = pet_cext_big_tile(header, (size_t) total, t, &o, &b) && o >= end && b >= 12 && b <= PET_CEXT_BIG_TILE_MAX_BYTES;
        if (ok) end = o + b;
      }
      if(ok){
        /* Cold validation shares the exact staging validator, but retains at
         * most one 64KiB tile workspace, never a full 1MiB tiled asset. */
        uint8_t *tile=heap_caps_malloc(PET_CEXT_BIG_TILE_MAX_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        FILE *file=tile?fopen(g_active.big_path[i],"rb"):NULL;ok=file!=NULL;
        for(unsigned t=0;ok&&t<count;t++){
          uint32_t o,bytes;ok=pet_cext_big_tile(header,(size_t)total,t,&o,&bytes);
          unsigned ew=w-(t%header[4])*tw,eh=h-(t/header[4])*th;if(ew>tw)ew=tw;if(eh>th)eh=th;
          ok=ok&&fseek(file,(long)o,SEEK_SET)==0&&fread(tile,1,bytes,file)==bytes
            &&pet_cext_big_image_valid(tile,bytes,ew,eh);
        }
        if(file&&fclose(file)!=0)ok=false;
        free(tile);
      }
      if (ok) { g_active.big_meta[i] = r->bigs[i]; g_active.big_meta[i].tile_w = tw; g_active.big_meta[i].tile_h = th; g_active.big_meta[i].cols = header[4]; g_active.big_meta[i].rows = header[5]; g_active.big_meta[i].bytes = (uint32_t) total; g_active.big_count++; }
    }
  }
#if PET_P4_WASM_AUDIO
  if (ok && r->speech_sha256[0]) {
    size_t size = 0;
    speech_path(slot, generation, false, path);
    g_active.speech = speech_read(path, &size);
    ok = g_active.speech && speech_matches(g_active.speech, size, r->speech_sha256);
    if (ok) g_active.speech_size = size;
  }
#endif
  if (ok) {
    g_pending.serial = ++g_prepare_serial; g_pending.generation = g_generation;
    g_pending.bank = target; g_pending.empty = false;
    *ticket = (pet_cmedia_ticket_t){g_pending.serial, g_generation};
  } else bank_free(&g_active);
  g_current = previous; /* publication is exclusively commit's job */
  unlock();
  if (!ok) ESP_LOGW(TAG, "component media prepare failed slot=%u", (unsigned) slot);
  return ok;
}
static bool ticket_current(const pet_cmedia_ticket_t *ticket) {
  return ticket && ticket->serial && ticket->serial == g_pending.serial && ticket->generation == g_pending.generation && ticket->generation == g_generation;
}
bool pet_cmedia_commit(pet_cmedia_ticket_t *ticket) {
  lock();
  if (!ticket_current(ticket)) { unlock(); return false; }
  unsigned previous = g_current;
  if (!g_pending.empty) g_current = g_pending.bank;
  else bank_hide(&g_active);
  if (!g_banks[previous].pins) bank_free(&g_banks[previous]);
  g_active.generation = ++g_generation;
  memset(&g_pending, 0, sizeof(g_pending)); memset(ticket, 0, sizeof(*ticket));
  unlock(); return true;
}
bool pet_cmedia_abort(pet_cmedia_ticket_t *ticket) {
  lock();
  if (!ticket_current(ticket)) { unlock(); return false; }
  if (!g_pending.empty) bank_free(&g_banks[g_pending.bank]);
  memset(&g_pending, 0, sizeof(g_pending)); memset(ticket, 0, sizeof(*ticket));
  unlock(); return true;
}
static uint64_t pin_locked(void) {
  if (g_token == CMEDIA_RELEASED - 1) return 0; /* never reuse a token */
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) if (!g_leases[i].token) {
    g_leases[i].bank = g_current; g_active.pins++;
    atomic_store(&g_leases[i].token, ++g_token);
    return g_token;
  }
  return 0;
}
#if PET_P4_WASM_AUDIO
bool pet_cmedia_speech_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_speech_lease_t *out) {
  if (!out) return false;
  *out = (pet_cmedia_speech_lease_t){0};
  if (!expected || !try_lock()) return false;
  uint32_t offset, length;
  if (expected == g_generation &&
      pet_cext_speech_cue(g_active.speech, g_active.speech_size, index, &offset, &length)) {
    out->token = pin_locked();
    if (out->token) {
      out->data = g_active.speech + offset;
      out->bytes = length;
    }
  }
  unlock();
  return out->token != 0;
}
#endif

bool pet_cmedia_sample_pin(unsigned index, pet_cmedia_sample_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out)); lock();
  if (index < g_active.sample_count && g_active.samples) {
    out->token = pin_locked();
    if (out->token) { out->pcm = (const int16_t *)(g_active.samples + g_active.sample_offset[index]); out->frames = g_active.sample_frames[index]; out->generation = g_active.generation; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_clip_pin(unsigned index, pet_cmedia_clip_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out)); lock();
  if (!index && g_active.clip_ready) {
    out->token = pin_locked();
    if (out->token) { out->stream = g_active.clip + PET_CEXT_CLIP_HEADER; out->bytes = g_active.clip_bytes - PET_CEXT_CLIP_HEADER; out->meta = g_active.clip_meta; out->generation = g_active.generation; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_sample_pin_at(unsigned index, uint32_t expected, pet_cmedia_sample_lease_t *out) {
  if (!pet_cmedia_sample_pin(index, out)) return false;
  if (expected && out->generation == expected) return true;
  (void)pet_cmedia_unpin(out->token); memset(out, 0, sizeof(*out)); return false;
}
bool pet_cmedia_clip_pin_at(unsigned index, uint32_t expected, pet_cmedia_clip_lease_t *out) {
  if (!pet_cmedia_clip_pin(index, out)) return false;
  if (expected && out->generation == expected) return true;
  (void)pet_cmedia_unpin(out->token); memset(out, 0, sizeof(*out)); return false;
}
bool pet_cmedia_sample_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_sample_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!expected || !try_lock()) return false;
  if (expected == g_active.generation && index < g_active.sample_count && g_active.samples) {
    out->token = pin_locked();
    if (out->token) { out->pcm = (const int16_t *)(g_active.samples + g_active.sample_offset[index]); out->frames = g_active.sample_frames[index]; out->generation = expected; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_clip_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_clip_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!expected || !try_lock()) return false;
  if (expected == g_active.generation && !index && g_active.clip_ready) {
    out->token = pin_locked();
    if (out->token) { out->stream = g_active.clip + PET_CEXT_CLIP_HEADER; out->bytes = g_active.clip_bytes - PET_CEXT_CLIP_HEADER; out->meta = g_active.clip_meta; out->generation = expected; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_unpin(uint64_t token) {
  if (!token) return false;
  lock();
  if (token >= CMEDIA_RELEASED) { unlock(); return false; }
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) {
    uint32_t expected = (uint32_t)token;
    if (!atomic_compare_exchange_strong(&g_leases[i].token, &expected, 0)) continue;
    unsigned bank = g_leases[i].bank;
    g_reclaimed++;
    if (--g_banks[bank].pins == 0 && (bank != g_current || !bank_visible(&g_active))) bank_free(&g_banks[bank]);
    unlock(); return true;
  }
  unlock(); return false;
}
/* Compatibility entry points fail closed: an untracked borrow is never returned. */
const int16_t *pet_cmedia_sample_pcm(unsigned index, uint32_t *frames) {
  (void)index; if (frames) *frames = 0; return NULL;
}
const uint8_t *pet_cmedia_clip_stream(unsigned index, size_t *bytes, pet_cext_clip_t *meta) {
  (void)index; if (bytes) *bytes = 0; if (meta) memset(meta, 0, sizeof(*meta)); return NULL;
}
bool pet_cmedia_big_meta(unsigned big, pet_cext_big_t *meta) { return pet_cmedia_big_meta_at(big, 0, meta); }
bool pet_cmedia_big_meta_at(unsigned big, uint32_t expected, pet_cext_big_t *meta) {
  lock(); bool ok = (!expected || expected == g_generation) && big < g_active.big_count; if (ok && meta) *meta = g_active.big_meta[big]; unlock(); return ok;
}
bool pet_cmedia_big_tile(unsigned big, unsigned tile, uint8_t *out, size_t capacity, size_t *bytes) { return pet_cmedia_big_tile_at(big, tile, 0, out, capacity, bytes); }
bool pet_cmedia_big_tile_at(unsigned big, unsigned tile, uint32_t expected, uint8_t *out, size_t capacity, size_t *bytes) {
  if (bytes) *bytes = 0;
  lock();
  bool ok = (!expected || expected == g_generation) && big < g_active.big_count && g_active.big_table[big] && out;
  uint32_t offset = 0, size = 0; char path[32];
  if (ok) { ok = pet_cext_big_tile(g_active.big_table[big], g_active.big_meta[big].bytes, tile, &offset, &size) && size <= capacity; strcpy(path, g_active.big_path[big]); }
  if (!ok) { unlock(); return false; }
  FILE *f = fopen(path, "rb"); if (!f) { unlock(); return false; }
  ok = fseek(f, (long) offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size; fclose(f);
  unlock();
  if (ok && bytes) *bytes = size;
  return ok;
}
bool pet_cmedia_big_meta_try_at(unsigned big, uint32_t expected, pet_cext_big_t *meta) {
  if (!meta || !expected || !try_lock()) return false;
  bool ok = expected == g_generation && big < g_active.big_count;
  if (ok) *meta = g_active.big_meta[big];
  unlock(); return ok;
}
bool pet_cmedia_big_tile_try_at(unsigned big, unsigned tile, uint32_t expected, uint8_t *out, size_t capacity, size_t *bytes) {
  if (bytes) *bytes = 0;
#ifdef ESP_PLATFORM
  /* Admission precedes the store lock: never wait behind an admitted writer.
   * Keep occupancy through fclose. Broker calls hold only their short mux. */
  if (!expected || !out) return false;
  pet_resource_owner_t owner = {0}; pet_resource_lease_t lease = {0};
  if (pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK), &owner) != PET_RESOURCE_OK) return false;
  if (pet_resources_device_acquire(owner, PET_RESOURCE_STORAGE_WORK, 1, &lease) != PET_RESOURCE_OK) { pet_resources_device_revoke(owner); return false; }
  bool ok = false;
  if (try_lock()) {
    uint32_t offset = 0, size = 0;
    if (expected == g_generation && big < g_active.big_count && g_active.big_table[big]
        && pet_cext_big_tile(g_active.big_table[big], g_active.big_meta[big].bytes, tile, &offset, &size)
        && size <= capacity && size <= PET_CEXT_BIG_TILE_MAX_BYTES && pet_resources_device_current(lease)) {
      FILE *f = fopen(g_active.big_path[big], "rb");
      if (f) { ok = fseek(f, (long)offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size; fclose(f); }
      if (ok && bytes) *bytes = size;
    }
    unlock();
  }
  (void)pet_resources_device_finish(lease); pet_resources_device_revoke(owner);
  return ok;
#else
  /* Host callers need an explicit broker double; never bypass admission. */
  (void)big; (void)tile; (void)expected; (void)out; (void)capacity; return false;
#endif
}
uint32_t pet_cmedia_generation(void) { lock(); uint32_t g = g_generation; unlock(); return g; }

bool pet_cmedia_stats(pet_cmedia_stats_t *out) {
  if (!out) return false;
  lock(); memset(out, 0, sizeof(*out));
  out->generation = g_generation; out->tokens_issued = g_token;
  out->tokens_reclaimed = g_reclaimed; out->deferred_reclaimed = g_deferred_reclaimed;
  for (unsigned i = 0; i < PET_CMEDIA_BANKS; i++) {
    const cmedia_bank_t *b = &g_banks[i]; bool live = b->samples || b->clip;
#if PET_P4_WASM_AUDIO
    live = live || b->speech;
#endif
    if (b->samples) out->sample_alloc_bytes += PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER;
    if (b->clip) out->clip_alloc_bytes += PET_CEXT_CLIP_MAX_BYTES;
    for (unsigned j = 0; j < PET_CEXT_BIG_MAX; j++) if (b->big_table[j]) {
      live = true; out->table_alloc_bytes += PET_CEXT_BIG_HEADER + PET_CEXT_BIG_MAX_TILES * 8;
    }
    out->live_banks += live; out->pinned_banks += b->pins != 0; out->bank_pins += b->pins;
  }
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) out->live_tokens += atomic_load(&g_leases[i].token) != 0;
  unlock(); return true;
}
