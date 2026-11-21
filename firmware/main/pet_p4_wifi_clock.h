#pragma once

#include <stdbool.h>
#include <stdint.h>

#if PET_P4_SYSTEM_WIFI
/* Only the Wi-Fi task starts SNTP and accesses NVS. Readers use a short snapshot. */
void pet_wifi_clock_load_offset(void);
void pet_wifi_clock_service(bool online);
void pet_wifi_clock_note_offset(int32_t minutes);
/* Automatic mode follows the latest desktop offset, defaulting to UTC. */
enum { PET_WIFI_TZ_MIN = -840, PET_WIFI_TZ_MAX = 840, PET_WIFI_TZ_AUTO = 841 };
int32_t pet_wifi_clock_timezone(void);
int32_t pet_wifi_clock_auto_offset(void);
bool pet_wifi_clock_set_timezone(int32_t minutes);
bool pet_wifi_clock_read(uint64_t now_ms, uint64_t *utc_ms, int32_t *offset_minutes,
                         uint64_t *age_ms);
#endif
