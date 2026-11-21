#include "pet_p4_spiffs_hint.h"
#if PET_P4_SPIFFS_HINT_ENABLE
#include "spiffs.h"
#include "spiffs_nucleus.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#ifndef PET_SPIFFS_HINT_HOST_TEST
#include "esp_idf_version.h"
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || (ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1) && \
    !(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4) && \
      CONFIG_ESP_REV_MIN_FULL == 300 && CONFIG_ESP_REV_MAX_FULL == 399 && !CONFIG_SECURE_ENABLE_TEE))
#error "audit SPIFFS lookup ABI/callers before enabling on another SDK"
#endif
_Static_assert(sizeof(void*)==4,"audited P4 pointer width");
#if ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4)
_Static_assert(sizeof(spiffs_block_ix) == 2 && sizeof(spiffs_page_ix) == 2 && sizeof(int) == 4, "review SPIFFS lookup indices");
_Static_assert(__builtin_types_compatible_p(__typeof__(((spiffs *)0)->cursor_block_ix), spiffs_block_ix)
    && __builtin_types_compatible_p(__typeof__(((spiffs *)0)->cursor_obj_lu_entry), int), "review lookup cursor fields");
_Static_assert(__builtin_types_compatible_p(__typeof__(&spiffs_object_find_object_index_header_by_name), s32_t (*)(spiffs *, const u8_t *, spiffs_page_ix *)), "review lookup signature");
#endif
#endif
_Static_assert(SPIFFS_OBJ_NAME_LEN==32,"bounded name hash must match audited SDK");
enum {SETS=64, WAYS=4};
typedef struct {
    spiffs *fs; uint32_t hash;
    uint16_t block, entry; uint8_t length, valid;
} hint_t;
/* Four candidates per bounded critical section, not a 256-entry IRQ-off scan.
 * Coordinates are only consumed under the caller's real SPIFFS lock. The
 * metadata lock never covers SDK, FS, clock, heap, logging or kernel calls.
 * The same opaque mount address reused later is harmless: geometry is checked
 * against the live argument; the real visitor must still verify every name.
 */
/* Ordinary zero BSS remains internal SRAM; DRAM_ATTR would store 4160 zero
 * bytes in the OTA image. No external-RAM attribute or allocation here. */
static hint_t hints[SETS][WAYS];
static uint8_t replace[SETS];
static DRAM_ATTR portMUX_TYPE hint_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR pet_spiffs_hint_stats_t stats;
static void increment(uint32_t *value){if(*value<UINT32_MAX)(*value)++;}
static bool key(const u8_t *name,uint32_t *hash,uint8_t *length){
    if(!name)return false;
    uint32_t h=2166136261U;
    for(unsigned n=0;n<SPIFFS_OBJ_NAME_LEN;n++){
        if(!name[n]){*hash=h;*length=(uint8_t)n;return n!=0;}
        h=(h^name[n])*16777619U;
    }
    return false; /* Never scan beyond the SDK name bound. */
}
static bool geometry(spiffs *fs,uint16_t block,uint16_t entry){
    /* Only called with a live SDK fs under its real lock. */
    return fs && SPIFFS_CFG_LOG_PAGE_SZ(fs)>0 && SPIFFS_CFG_LOG_BLOCK_SZ(fs)>=SPIFFS_CFG_LOG_PAGE_SZ(fs)
        && block<fs->block_count && entry<SPIFFS_OBJ_LOOKUP_MAX_ENTRIES(fs);
}
/* Caller holds hint_mux. */
static void remember_hint(hint_t learned) {
    unsigned set = learned.hash & (SETS - 1);
    unsigned slot = WAYS;
    for (unsigned way = 0; way < WAYS; way++) {
        hint_t *h = &hints[set][way];
        if (h->valid && h->fs == learned.fs && h->hash == learned.hash && h->length == learned.length) {
            slot = way;
            break;
        }
        if (!h->valid) slot = way;
    }
    if (slot == WAYS) {
        slot = replace[set];
        replace[set] = (uint8_t)((slot + 1) % WAYS);
    }
    hints[set][slot] = learned;
}

#if PET_P4_SPIFFS_HINT_APPLY
typedef struct {
    int64_t deadline_us;
    unsigned visited;
} catalog_hint_scan_t;
static bool catalog_hints_attempted;

/* The SDK lookup caller already holds the filesystem lock. Inspect only
 * finalized index headers and keep only search coordinates. The subsequent
 * ordinary lookup still checks the full filename and returns its own result. */
static s32_t catalog_hint_visitor(spiffs *fs, spiffs_obj_id id, spiffs_block_ix block,
    int entry, const void *unused, void *context) {
    (void)unused;
    catalog_hint_scan_t *scan = context;
    if ((scan->visited++ & 63U) == 0 && esp_timer_get_time() >= scan->deadline_us)
        return SPIFFS_VIS_END;
    if (id == SPIFFS_OBJ_ID_FREE || id == SPIFFS_OBJ_ID_DELETED || !(id & SPIFFS_OBJ_ID_IX_FLAG))
        return SPIFFS_VIS_COUNTINUE;

    const spiffs_page_ix page = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, block, entry);
    spiffs_page_object_ix_header header;
    s32_t result = _spiffs_rd(fs, SPIFFS_OP_T_OBJ_LU2 | SPIFFS_OP_C_READ,
        0, SPIFFS_PAGE_TO_PADDR(fs, page), sizeof(header), (u8_t *)&header);
    if (result != SPIFFS_OK) return result;
    const unsigned flags = SPIFFS_PH_FLAG_DELET | SPIFFS_PH_FLAG_FINAL | SPIFFS_PH_FLAG_IXDELE;
    if (header.p_hdr.span_ix != 0
        || (header.p_hdr.flags & flags) != (SPIFFS_PH_FLAG_DELET | SPIFFS_PH_FLAG_IXDELE))
        return SPIFFS_VIS_COUNTINUE;

    uint32_t hash;
    uint8_t length;
    if (memcmp(header.name, "/p4", 3) == 0 && key(header.name, &hash, &length)) {
        portENTER_CRITICAL(&hint_mux);
        remember_hint((hint_t){.fs=fs, .hash=hash, .length=length, .valid=1,
            .block=block, .entry=(uint16_t)entry});
        portEXIT_CRITICAL(&hint_mux);
    }
    return SPIFFS_VIS_COUNTINUE;
}

static void prime_catalog_hints(spiffs *fs, const u8_t *name, uint8_t length) {
    static const char first_catalog[] = "/p4-miniapps-0.json";
    if (!fs || length != sizeof(first_catalog) - 1 || memcmp(name, first_catalog, sizeof(first_catalog)))
        return;

    portENTER_CRITICAL(&hint_mux);
    bool prime = !catalog_hints_attempted;
    catalog_hints_attempted = true;
    portEXIT_CRITICAL(&hint_mux);
    if (!prime) return;

    const int64_t started = esp_timer_get_time();
    catalog_hint_scan_t scan = {.deadline_us = started + 850000};
    (void)spiffs_obj_lu_find_entry_visitor(fs, 0, 0, SPIFFS_VIS_NO_WRAP, 0,
        catalog_hint_visitor, NULL, &scan, NULL, NULL);
    const int64_t ended = esp_timer_get_time();
    portENTER_CRITICAL(&hint_mux);
    if (ended >= started) stats.catalog_prime_us = (uint32_t)(ended - started);
    portEXIT_CRITICAL(&hint_mux);
}
#endif

s32_t __real_spiffs_object_find_object_index_header_by_name(spiffs*,const u8_t[SPIFFS_OBJ_NAME_LEN],spiffs_page_ix*);
s32_t __wrap_spiffs_object_find_object_index_header_by_name(spiffs *fs,const u8_t name[SPIFFS_OBJ_NAME_LEN],spiffs_page_ix *pix){
    int entry_errno=errno;
    uint32_t hash=0;uint8_t length=0;
    bool keyed=key(name,&hash,&length),found=false;
    unsigned set=hash&(SETS-1);hint_t candidate={0};
    if(keyed){
#if PET_P4_SPIFFS_HINT_APPLY
        prime_catalog_hints(fs, name, length);
#endif
        portENTER_CRITICAL(&hint_mux);
        for(unsigned way=0;way<WAYS;way++){
            const hint_t *h=&hints[set][way];
            if(h->valid && h->fs==fs && h->hash==hash && h->length==length){candidate=*h;found=true;break;}
        }
        portEXIT_CRITICAL(&hint_mux);
    }
    found=found && geometry(fs,candidate.block,candidate.entry);
#if PET_P4_SPIFFS_HINT_APPLY
    if(found){fs->cursor_block_ix=candidate.block;fs->cursor_obj_lu_entry=candidate.entry;}
#endif
    int64_t begin=esp_timer_get_time();
    /* Exactly once, original pointer/arguments/error and actual media reads. */
    errno=entry_errno;
    s32_t result=__real_spiffs_object_find_object_index_header_by_name(fs,name,pix);
    int result_errno=errno;
    int64_t end=esp_timer_get_time();
    uint64_t elapsed=end>=begin?(uint64_t)(end-begin):0;
    hint_t learned={0};
    if(keyed && result==SPIFFS_OK && fs && fs->cursor_obj_lu_entry>=0
       && fs->cursor_obj_lu_entry<=UINT16_MAX
       && geometry(fs,(uint16_t)fs->cursor_block_ix,(uint16_t)fs->cursor_obj_lu_entry)){
        learned=(hint_t){.fs=fs,.hash=hash,.length=length,.valid=1,
          .block=(uint16_t)fs->cursor_block_ix,.entry=(uint16_t)fs->cursor_obj_lu_entry};
    }
    portENTER_CRITICAL(&hint_mux);
    increment(&stats.calls);if(found)increment(&stats.candidates);
#if PET_P4_SPIFFS_HINT_APPLY
    if(found)increment(&stats.applied);
#endif
    uint32_t bounded=elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;
    if(bounded>stats.maximum_us)stats.maximum_us=bounded;
    if(result==SPIFFS_OK){increment(&stats.positives);stats.positive_us+=elapsed;}
    else if(result==SPIFFS_ERR_NOT_FOUND){increment(&stats.negatives);stats.negative_us+=elapsed;}
    else{increment(&stats.errors);stats.error_us+=elapsed;}
    if (learned.valid) remember_hint(learned);
    portEXIT_CRITICAL(&hint_mux);
    errno=result_errno;
    return result;
}
void pet_spiffs_hint_stats(pet_spiffs_hint_stats_t *out){
    if(!out)return;
    portENTER_CRITICAL(&hint_mux);*out=stats;portEXIT_CRITICAL(&hint_mux);
}
#endif
