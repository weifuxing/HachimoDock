#pragma once
#include <stdint.h>
#include <string.h>

/* Fixed memory, O(1) recording. Percentiles are calculated on a caller-owned
 * snapshot outside realtime locks, only when diagnostics are requested. */
#define PET_LATENCY_SAMPLES 128
typedef struct {
    uint32_t values[PET_LATENCY_SAMPLES];
    uint16_t count, next;
} pet_latency_window_t;
static inline void pet_latency_record(pet_latency_window_t *w, uint64_t us) {
    w->values[w->next] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    w->next = (w->next + 1) % PET_LATENCY_SAMPLES;
    if (w->count < PET_LATENCY_SAMPLES) ++w->count;
}
static inline void pet_latency_percentiles(pet_latency_window_t *snapshot,
                                           uint32_t *p95, uint32_t *p99) {
    uint16_t n = snapshot->count;
    *p95 = *p99 = 0;
    if (!n) return;
    for (unsigned i = 1; i < n; ++i) {
        uint32_t v = snapshot->values[i]; unsigned j = i;
        while (j && snapshot->values[j-1] > v) {
            snapshot->values[j] = snapshot->values[j-1]; --j;
        }
        snapshot->values[j] = v;
    }
    *p95 = snapshot->values[(95U * n + 99) / 100 - 1];
    *p99 = snapshot->values[(99U * n + 99) / 100 - 1];
}
