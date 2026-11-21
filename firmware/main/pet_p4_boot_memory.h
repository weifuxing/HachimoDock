#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fixed boot-only records. No allocations, raw paths, reset of SDK low-water
 * counters, or instrumentation in audio/render callbacks. */
typedef enum {
  PET_MEMORY_BOOT_ENTRY,
  PET_MEMORY_BOOT_STORAGE,
  PET_MEMORY_BOOT_CATALOG,
  PET_MEMORY_BOOT_BUILTINS,
  PET_MEMORY_BOOT_MANIFEST,
  PET_MEMORY_BOOT_TRANSPORT,
  PET_MEMORY_BOOT_DISPLAY,
  PET_MEMORY_BOOT_AUDIO,
  PET_MEMORY_BOOT_FIRST_FRAME,
  PET_MEMORY_BOOT_RX_TASKS,
  PET_MEMORY_BOOT_AFE,
  PET_MEMORY_BOOT_READY,
  PET_MEMORY_BOOT_STAGE_COUNT
} pet_boot_stage_t;

typedef struct {
  uint32_t uptime_ms, free_internal, minimum_internal, largest_internal;
  uint32_t free_psram;
} pet_boot_memory_sample_t;

typedef struct {
  uint32_t recorded;
  pet_boot_memory_sample_t samples[PET_MEMORY_BOOT_STAGE_COUNT];
} pet_boot_memory_trace_t;

/* Caller serializes access. Preserve the first observation for each stage. */
static inline bool pet_boot_memory_record(pet_boot_memory_trace_t *trace,
    pet_boot_stage_t stage, pet_boot_memory_sample_t sample) {
  if ((unsigned)stage >= PET_MEMORY_BOOT_STAGE_COUNT) return false;
  uint32_t bit = 1U << stage;
  if (trace->recorded & bit) return false;
  trace->samples[stage] = sample;
  trace->recorded |= bit;
  return true;
}

void pet_p4_boot_memory_checkpoint(pet_boot_stage_t stage);
