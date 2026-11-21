#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Covers queued AND executing copies, not UART/DMA buffers. Call with the
 * transport metrics lock held. Small requests share FIFO ordering; this is
 * memory admission, not priority dispatch or a guarantee of free queue slots. */
#define PET_PROTOCOL_PENDING_BYTES (256U * 1024U)
#define PET_PROTOCOL_SMALL_RESERVE (16U * 1024U)
#define PET_PROTOCOL_SMALL_BYTES 2048U
typedef struct {
    uint32_t bytes, peak_bytes, rejected, allocation_failures, queue_full;
    uint32_t max_wait_us, max_handle_us;
} pet_protocol_budget_t;

static inline bool pet_protocol_budget_acquire(pet_protocol_budget_t *s, size_t bytes) {
    size_t limit = bytes <= PET_PROTOCOL_SMALL_BYTES ? PET_PROTOCOL_PENDING_BYTES
        : PET_PROTOCOL_PENDING_BYTES - PET_PROTOCOL_SMALL_RESERVE;
    if (!bytes || bytes > limit || s->bytes > limit - bytes) {
        if (s->rejected != UINT32_MAX) ++s->rejected;
        return false;
    }
    s->bytes += (uint32_t)bytes;
    if (s->bytes > s->peak_bytes) s->peak_bytes = s->bytes;
    return true;
}
static inline void pet_protocol_budget_release(pet_protocol_budget_t *s, size_t bytes) {
    s->bytes = bytes <= s->bytes ? s->bytes - (uint32_t)bytes : 0;
}
static inline uint32_t pet_protocol_duration(uint64_t us) {
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}
/* Snapshot exported by the transport owner; no allocation or serial output. */
pet_protocol_budget_t pet_p4_protocol_budget_snapshot(void);
