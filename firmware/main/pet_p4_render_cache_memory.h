#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Allocation-only accounting. Callers serialize these small state updates;
 * allocation/free and heap queries must remain outside the accounting lock. */
#define PET_P4_RENDER_CACHE_MEMORY 1
typedef struct {
  uint32_t stream_bytes, work_stream_bytes, stream_slots, generation, mutations;
} pet_render_cache_memory_t;

static inline void pet_render_cache_memory_begin(pet_render_cache_memory_t *s) {
  ++s->mutations;
  ++s->generation;
}
static inline void pet_render_cache_memory_end(pet_render_cache_memory_t *s,
    int32_t stream_delta, int32_t slot_delta, uint32_t work_capacity) {
  s->stream_bytes = (uint32_t)((int64_t)s->stream_bytes + stream_delta);
  s->stream_slots = (uint32_t)((int64_t)s->stream_slots + slot_delta);
  if (work_capacity != UINT32_MAX) s->work_stream_bytes = work_capacity;
  ++s->generation;
  --s->mutations;
}
static inline bool pet_render_cache_memory_stable(pet_render_cache_memory_t before,
    pet_render_cache_memory_t after) {
  return !before.mutations && !after.mutations && before.generation == after.generation;
}
