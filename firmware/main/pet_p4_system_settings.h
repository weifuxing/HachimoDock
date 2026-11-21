#pragma once

#include <string.h>

#include "pet_p4_device_wifi.h"
#include "pet_p4_protocol.h"

#if PET_P4_SYSTEM_WIFI
#if PET_P4_BOARD_PICO || PET_P4_RELEASE_PICO
#error "System Wi-Fi configuration requires the WiFi6 pin map"
#endif

#include "pet_p4_lan.h"
#include "pet_p4_private_feed.h"

#define PET_SETTINGS_PAGE "settings"
#define PET_SETTINGS_ACTION "system_settings"
/* The private feed action occupies the top right of the component canvas. */
enum { PET_ACCOUNT_X = 436, PET_ACCOUNT_Y = 8, PET_ACCOUNT_W = 196, PET_ACCOUNT_H = 56 };
static inline bool pet_account_button_hit(int x, int y) {
  return x >= PET_ACCOUNT_X && x < PET_ACCOUNT_X + PET_ACCOUNT_W &&
         y >= PET_ACCOUNT_Y && y < PET_ACCOUNT_Y + PET_ACCOUNT_H;
}

#define PET_SETTINGS_WIFI_ROWS 4
#define PET_SETTINGS_KEY_ROWS 5
enum { PET_SETTINGS_KEY_LEFT = 10, PET_SETTINGS_KEY_WIDTH = 620,
       PET_SETTINGS_KEY_TOP = 151, PET_SETTINGS_KEY_ROW_HEIGHT = 63,
       PET_SETTINGS_KEY_GAP = 5 };

typedef enum { PET_SETTINGS_HOME, PET_SETTINGS_WIFI, PET_SETTINGS_PASSWORD, PET_SETTINGS_LAN, PET_SETTINGS_TIMEZONE, PET_SETTINGS_BLE, PET_SETTINGS_FEED, PET_SETTINGS_FEED_ENTRY } pet_settings_screen_t;
typedef enum {
  PET_SETTINGS_UP, PET_SETTINGS_DOWN, PET_SETTINGS_LEFT, PET_SETTINGS_RIGHT,
  PET_SETTINGS_ENTER, PET_SETTINGS_BACK,
} pet_settings_input_t;

typedef struct {
  pet_settings_screen_t screen;
  pet_wifi_snapshot_t wifi;
  pet_lan_pair_view_t lan;
  pet_wifi_ap_t target;
  unsigned selected, list_start, key_row, key_column, password_length;
  unsigned keyboard;
  int32_t timezone_draft, timezone_effective;
  bool short_password, request_busy, unsupported;
  int ble_available;
  bool ble_paired, ble_pairing;
  unsigned ble_code;
  bool feed_can_save, feed_username, feed_submitted;
  pet_private_kind_t feed_kind;
  pet_private_result_t feed_result;
  char feed_host[128], feed_target[128];
} pet_settings_view_t;

static inline bool pet_settings_active(const pet_p4_runtime_state_t *state) {
  return state && strcmp(state->screen_page, PET_SETTINGS_PAGE) == 0;
}

static inline unsigned pet_settings_home_step(const pet_settings_view_t *view) {
  (void)view;
  return 88;
}

bool pet_settings_open(pet_p4_runtime_state_t *state, unsigned long long ts_ms);
void pet_settings_input(pet_p4_runtime_state_t *state, pet_settings_input_t input,
                        unsigned long long ts_ms);
void pet_settings_touch(pet_p4_runtime_state_t *state, int x, int y, unsigned long long ts_ms);
void pet_settings_wifi_swipe(pet_p4_runtime_state_t *state, int start_x, int start_y,
                             bool next, unsigned long long ts_ms);
void pet_settings_maintain(pet_p4_runtime_state_t *state, bool interrupted);
void pet_settings_snapshot(pet_settings_view_t *view);
unsigned pet_settings_key_columns(unsigned row);
const char *pet_settings_key_label(unsigned keyboard, unsigned row, unsigned column, char ascii[2]);
#endif
