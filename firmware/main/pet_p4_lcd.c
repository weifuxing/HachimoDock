/*
 * [Input] Waveshare panel/BSP operations, boot lifecycle, and RGB565 pixels.
 * [Output] First-frame LCD reveal using DCS on the fixed-backlight adapter.
 * [Pos] ESP32-P4 LCD hardware boundary.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 */

#include "pet_p4_lcd.h"
#if PET_P4_RENDER_LCD_SPLIT_PROBE
#include "esp_timer.h"
#include <string.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "pet-p4-lcd";
static const int LCD_BACKLIGHT_FORCE_LEVEL = 1;
static const TickType_t LCD_REFRESH_WAIT_TICKS = pdMS_TO_TICKS(250);

static esp_lcd_panel_handle_t g_panel_handle;
static esp_lcd_panel_io_handle_t g_lcd_io;
static SemaphoreHandle_t g_refresh_done_sem;
static esp_err_t g_backlight_status = ESP_ERR_INVALID_STATE;
static bool g_backlight_revealed;
static bool g_forced_backlight_logged;
static bool g_refresh_callback_logged;

#if PET_P4_SYSTEM_WIFI
static DRAM_ATTR _Atomic uint32_t lcd_phase, lcd_worker_active;
static DRAM_ATTR _Atomic uint32_t lcd_sequence, lcd_count, lcd_last_tick, lcd_last_context;

void IRAM_ATTR pet_p4_lcd_on_underrun(void) {
  /* Single interrupt producer. The reader must run outside the ISR. */
  atomic_fetch_add(&lcd_sequence, 1);
  atomic_store(&lcd_last_tick, xTaskGetTickCountFromISR());
  atomic_store(&lcd_last_context,
    atomic_load(&lcd_phase) | (atomic_load(&lcd_worker_active) << 8));
  atomic_fetch_add(&lcd_count, 1);
  atomic_fetch_add(&lcd_sequence, 1);
}

void pet_p4_lcd_render_phase(enum pet_lcd_phase phase) { atomic_store(&lcd_phase, phase); }
void pet_p4_lcd_video_worker(bool active) { atomic_store(&lcd_worker_active, active); }
uint32_t pet_p4_lcd_underrun_count(void) { return atomic_load(&lcd_count); }

pet_lcd_underrun_t pet_p4_lcd_underrun(void) {
  pet_lcd_underrun_t record;
  uint32_t sequence;
  uint32_t context;
  do {
    sequence = atomic_load(&lcd_sequence);
    record.count = atomic_load(&lcd_count);
    record.last_tick = atomic_load(&lcd_last_tick);
    context = atomic_load(&lcd_last_context);
  } while ((sequence & 1) || sequence != atomic_load(&lcd_sequence));
  record.last_phase = context & 255;
  record.video_worker = (context >> 8) != 0;
  return record;
}
#endif

static esp_err_t force_lcd_backlight_gpio(int level) {
  int backlight_gpio = BSP_LCD_BACKLIGHT;
  if (backlight_gpio < 0) return ESP_OK;
  gpio_config_t io_conf = {
    .pin_bit_mask = 1ULL << backlight_gpio,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure LCD backlight GPIO");
  ESP_RETURN_ON_ERROR(gpio_set_level(backlight_gpio, level), TAG, "force LCD backlight GPIO");
  if (level == LCD_BACKLIGHT_FORCE_LEVEL && !g_forced_backlight_logged) {
    ESP_LOGI(TAG, "LCD backlight revealed gpio=%d", LCD_BACKLIGHT_FORCE_LEVEL);
    g_forced_backlight_logged = true;
  }
  return ESP_OK;
}

static esp_err_t set_lcd_visible(bool visible) {
  if (BSP_LCD_BACKLIGHT < 0) {
    // Rev A has a permanently powered backlight. Gate the image using DCS.
    return esp_lcd_panel_disp_on_off(g_panel_handle, visible);
  }
  esp_err_t err = visible ? bsp_display_backlight_on() : bsp_display_backlight_off();
  esp_err_t force_err = force_lcd_backlight_gpio(visible ? LCD_BACKLIGHT_FORCE_LEVEL : 1 - LCD_BACKLIGHT_FORCE_LEVEL);
  return err == ESP_OK ? force_err : err;
}

esp_err_t pet_p4_lcd_prepare_boot(void) {
  g_backlight_revealed = false;
  g_backlight_status = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  return g_backlight_status;
}

static bool IRAM_ATTR lcd_refresh_done_callback(
  esp_lcd_panel_handle_t panel,
  esp_lcd_dpi_panel_event_data_t *edata,
  void *user_ctx
) {
  (void) panel;
  (void) edata;
  SemaphoreHandle_t sem = (SemaphoreHandle_t) user_ctx;
  BaseType_t task_awake = pdFALSE;
  if (sem) {
    xSemaphoreGiveFromISR(sem, &task_awake);
  }
  return task_awake == pdTRUE;
}

esp_err_t pet_p4_lcd_init(void) {
  if (g_panel_handle) return ESP_OK;

  ESP_LOGI(TAG, "Initialize HD28091C40-V1 LCD on ESP32-P4");
  ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &g_panel_handle, &g_lcd_io), TAG, "initialize display");
  if (!g_refresh_done_sem) {
    g_refresh_done_sem = xSemaphoreCreateBinary();
    if (!g_refresh_done_sem) return ESP_ERR_NO_MEM;
  }
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {
    .on_refresh_done = lcd_refresh_done_callback,
  };
  ESP_RETURN_ON_ERROR(
    esp_lcd_dpi_panel_register_event_callbacks(g_panel_handle, &callbacks, g_refresh_done_sem),
    TAG,
    "register LCD refresh callback"
  );
  if (!g_refresh_callback_logged) {
    ESP_LOGI(TAG, "LCD refresh_done callback registered");
    g_refresh_callback_logged = true;
  }
  ESP_LOGI(
    TAG,
    "LCD pixel format=%s bits=%d",
    BSP_LCD_COLOR_FORMAT == ESP_LCD_COLOR_FORMAT_RGB565 ? "RGB565" : "RGB888",
    BSP_LCD_BITS_PER_PIXEL
  );
  // Keep the initialized panel hidden until the first complete rendered frame.
  // The fixed-backlight adapter can only blank pixels after DCS is available.
  g_backlight_status = set_lcd_visible(false);
  if (g_backlight_status != ESP_OK) {
    ESP_LOGW(TAG, "display hide failed: %s", esp_err_to_name(g_backlight_status));
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_reveal(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (g_backlight_revealed) return g_backlight_status;
  g_backlight_status = set_lcd_visible(true);
  if (g_backlight_status == ESP_OK) {
    g_backlight_revealed = true;
  } else {
    ESP_LOGW(TAG, "display first-frame reveal failed: %s", esp_err_to_name(g_backlight_status));
  }
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_backlight_status(void) {
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_keep_awake(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!g_backlight_revealed) return ESP_ERR_INVALID_STATE;
  return g_backlight_status;
}

static esp_err_t lcd_draw_core(int x,int y,int width,int height,const uint16_t *pixels,pet_lcd_observation_t *out) {
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  if(out)memset(out,0,sizeof(*out));
#else
  (void)out;
#endif
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!pixels || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;

  if (g_refresh_done_sem) {
    while (xSemaphoreTake(g_refresh_done_sem, 0) == pdTRUE) {
    }
  }
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  int64_t submit_begin=out?esp_timer_get_time():0;
#endif
  esp_err_t draw_err = esp_lcd_panel_draw_bitmap(g_panel_handle, x, y, x + width, y + height, pixels);
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  int64_t submit_end=out?esp_timer_get_time():0;
  if(out){out->valid=true;out->submit_us=pet_lcd_elapsed(submit_begin,submit_end,&out->valid);out->submitted=draw_err==ESP_OK;}
#endif
  if (draw_err != ESP_OK) return draw_err;
  bool timed_out=g_refresh_done_sem&&xSemaphoreTake(g_refresh_done_sem,LCD_REFRESH_WAIT_TICKS)!=pdTRUE;
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  if(out&&g_refresh_done_sem){out->refresh_waited=true;out->wait_us=pet_lcd_elapsed(submit_end,esp_timer_get_time(),&out->valid);}
#endif
  if (timed_out) {
    ESP_LOGW(TAG, "LCD refresh wait timed out");
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_draw_rgb565(int x,int y,int width,int height,const uint16_t *pixels){
  return lcd_draw_core(x,y,width,height,pixels,NULL);
}
#if PET_P4_RENDER_LCD_SPLIT_PROBE
esp_err_t pet_p4_lcd_draw_rgb565_observed(int x,int y,int width,int height,const uint16_t *pixels,pet_lcd_observation_t *out){
  return lcd_draw_core(x,y,width,height,pixels,out);
}
#endif

int pet_p4_lcd_scan_i2c(char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) return 0;
  buffer[0] = '\0';

  esp_err_t init_err = bsp_i2c_init();
  if (init_err != ESP_OK) {
    snprintf(buffer, buffer_size, "init:%s", esp_err_to_name(init_err));
    return 0;
  }

  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (!bus) {
    snprintf(buffer, buffer_size, "no-bus");
    return 0;
  }

  int found = 0;
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr += 1) {
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
    int written = snprintf(
      buffer + used,
      buffer_size - used,
      "%s0x%02X",
      found == 0 ? "" : ",",
      addr
    );
    if (written < 0 || (size_t) written >= buffer_size - used) {
      buffer[buffer_size - 1] = '\0';
      break;
    }
    used += (size_t) written;
    found += 1;
  }

  ESP_LOGI(TAG, "I2C scan found %d device(s): %s", found, found > 0 ? buffer : "none");
  return found;
}
