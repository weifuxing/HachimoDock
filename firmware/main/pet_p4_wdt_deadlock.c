#include "pet_p4_wdt_deadlock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#define DEADLOCK_MS 30000U
static _Atomic uint32_t last_heartbeat[PET_DEADLOCK_TASK_COUNT];
static _Atomic uint32_t enabled_at;

static uint32_t clock_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000) | 1U;
}

void pet_p4_wdt_deadlock_feed(pet_p4_deadlock_task_t task) {
  atomic_store_explicit(&last_heartbeat[task], clock_ms(), memory_order_relaxed);
}

static void check_heartbeats(void *context) {
  (void)context;
  uint32_t start = atomic_load_explicit(&enabled_at, memory_order_relaxed);
  uint32_t now = clock_ms();
  if (now - start <= DEADLOCK_MS) return;
  for (unsigned task = 0; task < PET_DEADLOCK_TASK_COUNT; ++task) {
    uint32_t last = atomic_load_explicit(&last_heartbeat[task], memory_order_relaxed);
    // A task may feed between the first time sample and this load.
    if (last && clock_ms() - last > DEADLOCK_MS) {
      ESP_LOGE("wdt-deadlock", "task %u missed its heartbeat", task);
      abort();
    }
  }
}

void pet_p4_wdt_deadlock_start(void) {
  esp_timer_handle_t timer;
  esp_timer_create_args_t args = {
    .callback = check_heartbeats,
    .name = "p4_deadlock",
    .skip_unhandled_events = true,
  };
  atomic_store_explicit(&enabled_at, clock_ms(), memory_order_relaxed);
  if (esp_timer_create(&args, &timer) == ESP_OK
      && esp_timer_start_periodic(timer, 1000000) != ESP_OK) {
    esp_timer_delete(timer);
  }
}
