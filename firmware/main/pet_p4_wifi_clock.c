#include "pet_p4_wifi_clock.h"

#if PET_P4_SYSTEM_WIFI
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <sys/time.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define RTC_NOINIT_ATTR
#endif

#define CLOCK_NAMESPACE "pet_wifi_clock"
#define OFFSET_KEY "utc_offset"
#define TIMEZONE_KEY "timezone"
#define CLOCK_RETRY_MS 30000
#define CLOCK_EPOCH_MIN 1577836800LL
#define CLOCK_EPOCH_MAX 4102444799LL

static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t synced_utc_ms;
static uint64_t synced_at_ms;
static uint64_t age_at_anchor_ms;
/* Stored only in the retained RTC domain. A cold boot must not infer
 * current time from a flash timestamp when the oscillator stopped. */
static RTC_NOINIT_ATTR uint32_t retained_sync_sec;
static RTC_NOINIT_ATTR uint32_t retained_sync_check;
static int32_t utc_offset_minutes;
static bool have_offset;
static bool offset_dirty;
static int32_t timezone_minutes = PET_WIFI_TZ_AUTO;
static bool timezone_dirty;
static uint64_t timezone_save_after_ms;
static bool sntp_running;
static uint64_t retry_after_ms;
static uint64_t save_after_ms;

static bool offset_valid(int32_t minutes) {
  return minutes >= PET_WIFI_TZ_MIN && minutes <= PET_WIFI_TZ_MAX;
}

static bool wall_time_valid(const struct timeval *time) {
  return time->tv_sec >= CLOCK_EPOCH_MIN && time->tv_sec <= CLOCK_EPOCH_MAX &&
    time->tv_usec >= 0 && time->tv_usec < 1000000;
}

static void restore_retained_time(void) {
  const uint32_t calibrated = retained_sync_sec;
  if (calibrated < CLOCK_EPOCH_MIN || calibrated > CLOCK_EPOCH_MAX ||
      retained_sync_check != ~calibrated) return;
  struct timeval wall;
  if (gettimeofday(&wall, NULL) != 0 || !wall_time_valid(&wall) ||
      wall.tv_sec < calibrated) return;
  const uint64_t mono = esp_timer_get_time() / 1000ULL;
  const uint64_t utc = (uint64_t)wall.tv_sec * 1000 + (uint64_t)wall.tv_usec / 1000;
  portENTER_CRITICAL(&clock_lock);
  if (!synced_utc_ms) {
    synced_utc_ms = utc;
    synced_at_ms = mono;
    age_at_anchor_ms = utc - (uint64_t)calibrated * 1000;
  }
  portEXIT_CRITICAL(&clock_lock);
}

void pet_wifi_clock_note_offset(int32_t minutes) {
  if (!offset_valid(minutes)) return;
  portENTER_CRITICAL(&clock_lock);
  if (!have_offset || utc_offset_minutes != minutes) {
    utc_offset_minutes = minutes;
    have_offset = true;
    offset_dirty = true;
  }
  portEXIT_CRITICAL(&clock_lock);
}

int32_t pet_wifi_clock_timezone(void) {
  portENTER_CRITICAL(&clock_lock);
  const int32_t minutes = timezone_minutes;
  portEXIT_CRITICAL(&clock_lock);
  return minutes;
}

static int32_t display_offset_locked(void) {
  return timezone_minutes == PET_WIFI_TZ_AUTO ? utc_offset_minutes : timezone_minutes;
}

int32_t pet_wifi_clock_auto_offset(void) {
  portENTER_CRITICAL(&clock_lock);
  const int32_t minutes = utc_offset_minutes;
  portEXIT_CRITICAL(&clock_lock);
  return minutes;
}

bool pet_wifi_clock_set_timezone(int32_t minutes) {
  if (minutes != PET_WIFI_TZ_AUTO && !offset_valid(minutes)) return false;
  portENTER_CRITICAL(&clock_lock);
  if (timezone_minutes != minutes) {
    timezone_minutes = minutes;
    timezone_dirty = true;
  }
  portEXIT_CRITICAL(&clock_lock);
  return true;
}

void pet_wifi_clock_load_offset(void) {
  restore_retained_time();
  nvs_handle_t nvs;
  if (nvs_open(CLOCK_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
  int32_t minutes, timezone;
  const bool offset_loaded = nvs_get_i32(nvs, OFFSET_KEY, &minutes) == ESP_OK && offset_valid(minutes);
  const bool timezone_loaded = nvs_get_i32(nvs, TIMEZONE_KEY, &timezone) == ESP_OK &&
    (timezone == PET_WIFI_TZ_AUTO || offset_valid(timezone));
  nvs_close(nvs);

  portENTER_CRITICAL(&clock_lock);
  if (timezone_loaded && !timezone_dirty) timezone_minutes = timezone;
  if (offset_loaded) {
    if (!have_offset) {
      utc_offset_minutes = minutes;
      have_offset = true;
    } else if (utc_offset_minutes == minutes) {
      offset_dirty = false;
    }
  }
  portEXIT_CRITICAL(&clock_lock);
}

static void save_if_changed(uint64_t now_ms, uint64_t *retry_after, const char *key,
                            const int32_t *value, bool *dirty, bool rate_limit) {
  if (now_ms < *retry_after) return;
  portENTER_CRITICAL(&clock_lock);
  const bool needs_save = *dirty;
  const int32_t minutes = *value;
  portEXIT_CRITICAL(&clock_lock);
  if (!needs_save) return;

  nvs_handle_t nvs;
  esp_err_t result = nvs_open(CLOCK_NAMESPACE, NVS_READWRITE, &nvs);
  if (result == ESP_OK) {
    result = nvs_set_i32(nvs, key, minutes);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
  }
  *retry_after = result == ESP_OK && !rate_limit ? 0 : now_ms + CLOCK_RETRY_MS;
  if (result != ESP_OK) return;
  portENTER_CRITICAL(&clock_lock);
  if (*value == minutes) *dirty = false;
  portEXIT_CRITICAL(&clock_lock);
}

static void time_synced(struct timeval *time) {
  if (!time || !wall_time_valid(time)) return;
  const uint64_t now_ms = esp_timer_get_time() / 1000ULL;
  const uint64_t utc_ms = (uint64_t)time->tv_sec * 1000 + (uint64_t)time->tv_usec / 1000;
  portENTER_CRITICAL(&clock_lock);
  synced_utc_ms = utc_ms;
  synced_at_ms = now_ms;
  age_at_anchor_ms = 0;
  retained_sync_sec = (uint32_t)time->tv_sec;
  retained_sync_check = ~retained_sync_sec;
  portEXIT_CRITICAL(&clock_lock);
}

void pet_wifi_clock_service(bool online) {
  const uint64_t now_ms = esp_timer_get_time() / 1000ULL;
  save_if_changed(now_ms, &timezone_save_after_ms, TIMEZONE_KEY, &timezone_minutes, &timezone_dirty, false);
  save_if_changed(now_ms, &save_after_ms, OFFSET_KEY, &utc_offset_minutes, &offset_dirty, true);
  if (!online) {
    if (sntp_running) {
      esp_netif_sntp_deinit();
      sntp_running = false;
    }
    retry_after_ms = 0;
    return;
  }
  if (sntp_running || now_ms < retry_after_ms) return;
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  config.wait_for_sync = false;
  config.sync_cb = time_synced;
  sntp_running = esp_netif_sntp_init(&config) == ESP_OK;
  retry_after_ms = now_ms + CLOCK_RETRY_MS;
}

bool pet_wifi_clock_read(uint64_t now_ms, uint64_t *utc_ms, int32_t *offset_minutes,
                         uint64_t *age_ms) {
  portENTER_CRITICAL(&clock_lock);
  const bool valid = synced_utc_ms != 0 && now_ms >= synced_at_ms;
  if (valid) {
    const uint64_t elapsed = now_ms - synced_at_ms;
    *age_ms = age_at_anchor_ms + elapsed;
    *utc_ms = synced_utc_ms + elapsed;
    *offset_minutes = display_offset_locked();
  }
  portEXIT_CRITICAL(&clock_lock);
  return valid;
}
#endif
