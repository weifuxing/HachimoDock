#include "pet_p4_system_settings.h"

#if PET_P4_SYSTEM_WIFI
#include <stdio.h>

#include "pet_p4_audio.h"
#include "pet_p4_ble_shortcut.h"
#include "pet_p4_ota.h"
#include "pet_p4_miniapp.h"
#include "mbedtls/platform_util.h"
#include "pet_p4_wifi_clock.h"
#include "pet_p4_wifi_data.h"

static pet_settings_view_t page;
static char return_page[PET_P4_PAGE_MAX];
static char password[PET_PRIVATE_TOKEN_MAX + 1];
static pet_private_feed_t selected_feed;
static pet_private_secret_t feed_secret;
static bool page_open;

static void clear_password(void) {
  volatile char *p = password;
  for (unsigned i = 0; i < sizeof(password); ++i) p[i] = 0;
  page.password_length = 0;
}

bool pet_settings_open(pet_p4_runtime_state_t *state, unsigned long long ts_ms) {
  if (!state || pet_settings_active(state) || !pet_p4_is_wifi6_hardware() ||
      state->approval_active || state->asset_transfer_active || pet_p4_ota_transfer_active() ||
      state->session_voice_active || pet_p4_audio_active()) return false;
  const bool component = !strcmp(state->screen_page, "app");
  if (!component && strcmp(state->screen_page, "main") && strcmp(state->screen_page, "components"))
    return false;
  clear_password();
  memset(&page, 0, sizeof(page));
  pet_wifi_endpoint_t endpoint;
  if (component) {
    if (!pet_p4_miniapp_private_feed(&selected_feed, &endpoint)) return false;
    page.screen = PET_SETTINGS_FEED;
    page.feed_can_save = pet_private_storage_ready();
    page.feed_kind = selected_feed.kind;
    snprintf(page.feed_host, sizeof(page.feed_host), "%s", endpoint.host);
    const char *target = page.feed_kind == PET_PRIVATE_MQTT ? selected_feed.topic : endpoint.path;
    snprintf(page.feed_target, sizeof(page.feed_target), "%.*s", (int)sizeof(page.feed_target) - 1, target);
  }
  page_open = true;
  snprintf(return_page, sizeof(return_page), "%s", state->screen_page);
  snprintf(state->screen_page, sizeof(state->screen_page), "%s", PET_SETTINGS_PAGE);
  state->last_update_ms = ts_ms;
  return true;
}

static unsigned list_limit(void) {
  return page.wifi.count + (page.wifi.saved ? 1 : 0);
}

static void update_list(void) {
  pet_device_wifi_snapshot(&page.wifi);
  if (page.screen != PET_SETTINGS_WIFI) return;
  if (page.selected > list_limit()) page.selected = list_limit();
  if (page.selected < page.wifi.count)
    page.list_start = page.selected / PET_SETTINGS_WIFI_ROWS * PET_SETTINGS_WIFI_ROWS;
  else if (page.list_start >= page.wifi.count)
    page.list_start = 0;
}

static void move_wifi_page(bool next, bool wrap) {
  unsigned start = page.list_start;
  if (next) {
    start += PET_SETTINGS_WIFI_ROWS;
    if (start >= page.wifi.count) {
      if (!wrap) return;
      start = 0;
    }
  } else {
    if (!start) return;
    start -= PET_SETTINGS_WIFI_ROWS;
  }
  page.selected = start;
  update_list();
}

static void request(pet_wifi_operation_t operation) {
  page.request_busy = !pet_device_wifi_request(operation, &page.target, password);
}

static void open_wifi(void) {
  page.screen = PET_SETTINGS_WIFI;
  page.selected = 0;
  page.list_start = 0;
  request(PET_WIFI_SCAN);
}

static void select_network(void) {
  if (page.selected == page.wifi.count) {
    request(PET_WIFI_SCAN);
    return;
  }
  if (page.selected > page.wifi.count) {
    request(PET_WIFI_FORGET);
    return;
  }
  if (page.wifi.status == PET_WIFI_SCANNING || page.wifi.status == PET_WIFI_CONNECTING) return;
  page.target = page.wifi.aps[page.selected];
  page.unsupported = page.target.security == PET_WIFI_UNSUPPORTED;
  if (page.unsupported) return;
  clear_password();
  if (page.target.security == PET_WIFI_OPEN) {
    request(PET_WIFI_CONNECT);
    return;
  }
  page.short_password = false;
  page.keyboard = page.key_row = page.key_column = 0;
  page.screen = PET_SETTINGS_PASSWORD;
}

unsigned pet_settings_key_columns(unsigned row) {
  return row == 4 ? 5 : 10;
}

const char *pet_settings_key_label(unsigned keyboard, unsigned row, unsigned column, char ascii[2]) {
  static const char *const lower[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_@."};
  static const char *const upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_@."};
  static const char *const symbols[] = {"!#$%&*+=/\\", "[]{}()<>:;", "'\"?`~^|,._", "@-01234567"};
  if (row == 4) {
    static const char *const buttons[] = {"大小写", "符号", "空格", "删除", "连接"};
    return buttons[column];
  }
  const char *const *keys = keyboard == 2 ? symbols : keyboard == 1 ? upper : lower;
  ascii[0] = keys[row][column];
  ascii[1] = 0;
  return ascii;
}

static bool feed_apply(bool forget) {
  pet_private_feed_t current;
  pet_wifi_endpoint_t endpoint;
  if (!pet_p4_miniapp_private_feed(&current, &endpoint) ||
      memcmp(current.id, selected_feed.id, sizeof(current.id)) ||
      !pet_private_secret_schedule(&selected_feed, &feed_secret, forget)) return false;
  page.feed_submitted = true;
  mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
  return true;
}

static void activate_key(void) {
  page.short_password = false;
  if (page.key_row == 4) {
    if (page.key_column == 0) {
      page.keyboard = page.keyboard == 0 ? 1 : 0;
      return;
    }
    if (page.key_column == 1) {
      page.keyboard = page.keyboard == 2 ? 0 : 2;
      return;
    }
    if (page.key_column == 3) {
      if (page.password_length) password[--page.password_length] = 0;
      return;
    }
    if (page.key_column == 4) {
      if (page.screen == PET_SETTINGS_FEED_ENTRY) {
        if (page.feed_username) {
          memcpy(feed_secret.user, password, page.password_length + 1);
          clear_password();
          page.feed_username = false;
        } else {
          memcpy(feed_secret.token, password, page.password_length + 1);
          if (feed_apply(false)) {
            clear_password();
            page.screen = PET_SETTINGS_FEED;
            page.selected = 0;
          } else {
            mbedtls_platform_zeroize(feed_secret.token, sizeof(feed_secret.token));
            page.short_password = true;
          }
        }
        return;
      }
      if (page.password_length < 8) {
        page.short_password = true;
        return;
      }
      request(PET_WIFI_CONNECT);
      if (!page.request_busy) {
        clear_password();
        page.screen = PET_SETTINGS_WIFI;
      }
      return;
    }
  }
  const unsigned limit = page.screen != PET_SETTINGS_FEED_ENTRY ? PET_WIFI_PASSWORD_SIZE - 2 :
    page.feed_username ? PET_PRIVATE_USER_MAX : PET_PRIVATE_TOKEN_MAX;
  if (page.password_length >= limit) return;
  char ascii[2];
  const char key = page.key_row == 4 ? ' ' :
    pet_settings_key_label(page.keyboard, page.key_row, page.key_column, ascii)[0];
  password[page.password_length++] = key;
  password[page.password_length] = 0;
}

static void timezone_input(pet_settings_input_t input) {
  if (input == PET_SETTINGS_ENTER) {
    pet_wifi_clock_set_timezone(page.timezone_draft);
    page.screen = PET_SETTINGS_HOME;
    page.selected = 2;
    return;
  }
  if (input == PET_SETTINGS_UP || input == PET_SETTINGS_DOWN) {
    page.timezone_draft = page.timezone_draft == PET_WIFI_TZ_AUTO
      ? pet_wifi_clock_auto_offset() : PET_WIFI_TZ_AUTO;
  } else if (input == PET_SETTINGS_LEFT || input == PET_SETTINGS_RIGHT) {
    int32_t minutes = page.timezone_draft == PET_WIFI_TZ_AUTO
      ? pet_wifi_clock_auto_offset() : page.timezone_draft;
    minutes += input == PET_SETTINGS_LEFT ? -15 : 15;
    if (minutes < PET_WIFI_TZ_MIN) minutes = PET_WIFI_TZ_MIN;
    if (minutes > PET_WIFI_TZ_MAX) minutes = PET_WIFI_TZ_MAX;
    page.timezone_draft = minutes;
  }
}

void pet_settings_input(pet_p4_runtime_state_t *state, pet_settings_input_t input,
                        unsigned long long ts_ms) {
  if (!pet_settings_active(state)) return;
  update_list();
  if (input == PET_SETTINGS_BACK) {
    clear_password();
    mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
    if (page.screen == PET_SETTINGS_FEED_ENTRY) {
      page.screen = PET_SETTINGS_FEED;
      page.selected = 0;
    } else if (page.screen == PET_SETTINGS_FEED) {
      snprintf(state->screen_page, sizeof(state->screen_page), "%s", return_page);
    } else if (page.screen == PET_SETTINGS_PASSWORD) page.screen = PET_SETTINGS_WIFI;
    else if (page.screen == PET_SETTINGS_LAN || page.screen == PET_SETTINGS_TIMEZONE) {
      page.selected = page.screen == PET_SETTINGS_LAN ? 1 : 2;
      page.screen = PET_SETTINGS_HOME;
    }
    else if (page.screen == PET_SETTINGS_BLE) {
      pet_ble_shortcut_close_pair();
      page.screen = PET_SETTINGS_HOME;
      page.selected = 3;
    } else if (page.screen == PET_SETTINGS_WIFI) {
      pet_device_wifi_cancel();
      page.screen = PET_SETTINGS_HOME;
      page.selected = 0;
    } else snprintf(state->screen_page, sizeof(state->screen_page), "%s", return_page);
  } else if (page.screen == PET_SETTINGS_HOME) {
    if (input == PET_SETTINGS_UP && page.selected > 0) --page.selected;
    if (input == PET_SETTINGS_DOWN && page.selected < 3u) ++page.selected;
    if (input == PET_SETTINGS_ENTER) {
      if (page.selected == 0) open_wifi();
      else if (page.selected == 1) {
        page.screen = PET_SETTINGS_LAN;
        page.selected = 0;

      } else if (page.selected == 3) {
        page.screen = PET_SETTINGS_BLE;
        request(PET_WIFI_START);
      } else {
        page.screen = PET_SETTINGS_TIMEZONE;
        page.timezone_draft = pet_wifi_clock_timezone();
      }
    }
  } else if (page.screen == PET_SETTINGS_FEED) {
    if (input == PET_SETTINGS_UP || input == PET_SETTINGS_DOWN) page.selected = !page.selected;
    if (input == PET_SETTINGS_ENTER && page.feed_can_save &&
        pet_private_local_result() != PET_PRIVATE_BUSY) {
      if (page.selected) {
        page.request_busy = !feed_apply(true);
      } else {
        clear_password();
        mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
        page.feed_username = page.feed_kind == PET_PRIVATE_MQTT;
        page.short_password = false;
        page.request_busy = false;
        page.keyboard = page.key_row = page.key_column = 0;
        page.screen = PET_SETTINGS_FEED_ENTRY;
      }
    }
  } else if (page.screen == PET_SETTINGS_BLE) {
    if (input == PET_SETTINGS_ENTER && pet_ble_shortcut_available() > 0) {
      if (pet_ble_shortcut_paired()) pet_ble_shortcut_forget();
      else (void)pet_ble_keyboard_set_mode(true);
    }
  } else if (page.screen == PET_SETTINGS_TIMEZONE) {
    timezone_input(input);
  } else if (page.screen == PET_SETTINGS_LAN) {
    page.selected = 0;
    if (input == PET_SETTINGS_ENTER) {
      pet_lan_pair_view_t v;
      pet_p4_lan_pair_view(&v);
      if (v.paired && !v.busy) pet_p4_lan_forget();
    }
  } else if (page.screen == PET_SETTINGS_WIFI) {
    page.unsupported = false;
    if (input == PET_SETTINGS_UP && page.selected) --page.selected;
    if (input == PET_SETTINGS_DOWN && page.selected < list_limit()) ++page.selected;
    if (input == PET_SETTINGS_LEFT) move_wifi_page(false, false);
    if (input == PET_SETTINGS_RIGHT) move_wifi_page(true, false);
    if (input == PET_SETTINGS_ENTER) select_network();
    update_list();
  } else {
    unsigned columns = pet_settings_key_columns(page.key_row);
    if (input == PET_SETTINGS_ENTER) activate_key();
    if (input == PET_SETTINGS_LEFT && page.key_column) --page.key_column;
    if (input == PET_SETTINGS_RIGHT && page.key_column + 1 < columns) ++page.key_column;
    if (input == PET_SETTINGS_UP && page.key_row) --page.key_row;
    if (input == PET_SETTINGS_DOWN && page.key_row + 1 < PET_SETTINGS_KEY_ROWS) ++page.key_row;
    const unsigned next_columns = pet_settings_key_columns(page.key_row);
    if (next_columns != columns) page.key_column = page.key_column * next_columns / columns;
  }
  state->last_update_ms = ts_ms;
}

void pet_settings_wifi_swipe(pet_p4_runtime_state_t *state, int start_x, int start_y,
                             bool next, unsigned long long ts_ms) {
  if (!pet_settings_active(state) || state->approval_active || page.screen != PET_SETTINGS_WIFI ||
      start_x < 24 || start_x >= 616 || start_y < 126 || start_y >= 382) return;
  pet_settings_input(state, next ? PET_SETTINGS_RIGHT : PET_SETTINGS_LEFT, ts_ms);
}

void pet_settings_touch(pet_p4_runtime_state_t *state, int x, int y, unsigned long long ts_ms) {
  if (!pet_settings_active(state) || state->approval_active) return;
  if (x >= 16 && x < 125 && y >= 9 && y < 73) {
    pet_settings_input(state, PET_SETTINGS_BACK, ts_ms);
    return;
  }
  update_list();
  if (page.screen == PET_SETTINGS_HOME) {
    const unsigned step = pet_settings_home_step(&page);
    const unsigned count = 4;
    if (x >= 24 && x < 616 && y >= 103 && (unsigned)(y - 103) < count * step &&
        (unsigned)(y - 103) % step < step - 16) {
      page.selected = (unsigned)(y - 103) / step;
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_FEED) {
    if (x >= 24 && x < 616 && y >= 280 && y < 420 && (y - 280) % 70 < 58) {
      page.selected = (y - 280) / 70;
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_BLE) {
    if (x >= 24 && x < 616 && y >= 392 && y < 452) pet_settings_input(state,PET_SETTINGS_ENTER,ts_ms);
  } else if (page.screen == PET_SETTINGS_TIMEZONE) {
    if (y >= 160 && y < 232 && x >= 24 && x < 130)
      pet_settings_input(state, PET_SETTINGS_LEFT, ts_ms);
    else if (y >= 160 && y < 232 && x >= 510 && x < 616)
      pet_settings_input(state, PET_SETTINGS_RIGHT, ts_ms);
    else if (x >= 24 && x < 616 && y >= 260 && y < 330)
      pet_settings_input(state, PET_SETTINGS_UP, ts_ms);
    else if (x >= 24 && x < 616 && y >= 392 && y < 452)
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
  } else if (page.screen == PET_SETTINGS_LAN) {
    if (x < 24 || x >= 616) return;
    if (y >= 392 && y < 452) {
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_WIFI) {
    if (x >= 24 && x < 616 && y >= 126 && y < 382) {
      unsigned index = page.list_start + (unsigned)(y - 126) / 64;
      if (index < page.wifi.count) {
        page.selected = index;
        page.unsupported = false;
        select_network();
        state->last_update_ms = ts_ms;
      }
    } else if (y >= 395 && y < 455) {
      if (x >= 24 && x < 210) page.selected = page.wifi.count;
      else if (x >= 230 && x < 415 && page.wifi.saved) page.selected = page.wifi.count + 1;
      else if (x >= 435 && x < 616 && page.wifi.count > PET_SETTINGS_WIFI_ROWS) {
        move_wifi_page(true, true);
        state->last_update_ms = ts_ms;
        return;
      } else return;
      page.unsupported = false;
      select_network();
      state->last_update_ms = ts_ms;
    }
  } else if (x >= PET_SETTINGS_KEY_LEFT && x < PET_SETTINGS_KEY_LEFT + PET_SETTINGS_KEY_WIDTH &&
             y >= PET_SETTINGS_KEY_TOP && y < PET_SETTINGS_KEY_TOP + PET_SETTINGS_KEY_ROWS * PET_SETTINGS_KEY_ROW_HEIGHT) {
    page.key_row = (unsigned)(y - PET_SETTINGS_KEY_TOP) / PET_SETTINGS_KEY_ROW_HEIGHT;
    unsigned key_width = PET_SETTINGS_KEY_WIDTH / pet_settings_key_columns(page.key_row);
    page.key_column = (unsigned)(x - PET_SETTINGS_KEY_LEFT) / key_width;
    pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
  }
}

void pet_settings_maintain(pet_p4_runtime_state_t *state, bool interrupted) {
  static bool prompted;
  if (!pet_settings_active(state) && !strcmp(state->screen_page, "app") && !interrupted) {
    if (!pet_wifi_data_authorization_needed(NULL)) prompted = false;
    else if (!prompted) {
      pet_private_feed_t binding;
      pet_wifi_endpoint_t endpoint;
      if (pet_p4_miniapp_private_feed(&binding, &endpoint) &&
          pet_wifi_data_authorization_needed(&binding))
        prompted = pet_settings_open(state, state->last_update_ms + 1);
    }
  } else if (!pet_settings_active(state)) prompted = false;
  if (!page_open || (pet_settings_active(state) && !interrupted)) return;
  if (pet_settings_active(state))
    snprintf(state->screen_page, sizeof(state->screen_page), "%s", return_page);
  clear_password();
  mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
  pet_device_wifi_cancel();
  pet_ble_shortcut_close_pair();
  page_open = false;
}

void pet_settings_snapshot(pet_settings_view_t *view) {
  update_list();
  *view = page;
  if (view->screen == PET_SETTINGS_FEED && view->feed_submitted)
    view->feed_result = pet_private_local_result();
  if (view->screen == PET_SETTINGS_BLE) {
    view->ble_available = pet_ble_shortcut_available();
    view->ble_paired = pet_ble_shortcut_paired();
    view->ble_pairing = pet_ble_shortcut_pairing();
    view->ble_code = pet_ble_shortcut_code();
  }
  if (view->screen == PET_SETTINGS_TIMEZONE)
    view->timezone_effective = pet_wifi_clock_auto_offset();
  if (view->screen == PET_SETTINGS_LAN) {
    pet_p4_lan_pair_view(&view->lan);
  }
}
#endif
