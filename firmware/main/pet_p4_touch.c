#include "pet_p4_touch.h"
#include "pet_p4_system_settings.h"
#include "pet_p4_conversation.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "bsp/display.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pet_p4_assets.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_center_layout.h"
#include "pet_p4_touch_core.h"

#define PET_P4_TOUCH_SAMPLE_MS 15
#define PET_P4_TOUCH_SWIPE_THRESHOLD 48
#define PET_P4_TOUCH_LONG_PRESS_MS 700ULL
#define PET_P4_TOUCH_QUEUE_LENGTH 32
#define PET_P4_TOUCH_REACTION_MIN_MS 1200ULL
#define PET_P4_CST328_ADDRESS 0x1a

static const char *TAG = "pet-p4-touch";
static esp_lcd_touch_handle_t g_touch;
static esp_lcd_panel_io_handle_t g_touch_io;
static QueueHandle_t g_touch_queue;
static TaskHandle_t g_touch_task;
static bool g_touch_interrupt_mode;
static atomic_uint g_dropped_events;
static unsigned int g_event_sequence;
static unsigned int g_reaction_sequence;
static bool g_ready;
static const char *g_init_stage = "not_started";
static esp_err_t g_init_error, g_primary_error, g_backup_error;
static esp_err_t g_probe_15 = ESP_ERR_INVALID_STATE, g_probe_38 = ESP_ERR_INVALID_STATE, g_probe_10 = ESP_ERR_INVALID_STATE,
                 g_probe_18 = ESP_ERR_INVALID_STATE, g_probe_1a = ESP_ERR_INVALID_STATE;
static int g_address;
static bool g_ft6336;
static bool g_cst826;
static bool g_cst328;
static uint16_t g_chip_id;
static uint8_t g_vendor_id;
const char *pet_p4_touch_controller(void) {
  return g_cst328 ? "CST328" : g_cst826 ? "CST826" : g_ft6336 ? "FT6336U" : g_touch ? "GT911" : "unidentified";
}
static atomic_uint g_samples, g_contacts, g_poll_errors, g_consecutive_errors;
static atomic_int g_panel_x, g_panel_y, g_ui_x, g_ui_y;
static atomic_uint g_pointer_age_ms, g_stale_events;
typedef struct {
  bool has_pointer, has_gesture;
  pet_p4_pointer_event_t pointer;
  pet_p4_touch_event_t gesture;
} touch_sample_t;
static bool g_test_stroke;
static bool g_component_stroke;
static bool g_app_stroke;
#if PET_P4_SYSTEM_WIFI
enum { TOUCH_GENERAL, TOUCH_SETTINGS_ENTRY, TOUCH_SETTINGS_PAGE };
static atomic_int g_touch_context;
static bool g_account_stroke;
#endif

void pet_p4_touch_health(pet_p4_touch_health_t *out) {
  if (!out) return;
  *out = (pet_p4_touch_health_t){
    .stage = g_init_stage, .error = g_init_error, .address = g_address,
    .primary_error = g_primary_error, .backup_error = g_backup_error,
    .probe_15 = g_probe_15, .probe_38 = g_probe_38, .probe_10 = g_probe_10, .probe_18 = g_probe_18,
    .probe_1a = g_probe_1a,
    .interrupt_gpio = g_touch_interrupt_mode ? BSP_LCD_TOUCH_INT : GPIO_NUM_NC,
    .chip_id = g_chip_id, .vendor_id = g_vendor_id,
    .samples = atomic_load(&g_samples), .contacts = atomic_load(&g_contacts),
    .poll_errors = atomic_load(&g_poll_errors),
    .consecutive_errors = atomic_load(&g_consecutive_errors),
    .panel_x = atomic_load(&g_panel_x), .panel_y = atomic_load(&g_panel_y),
    .ui_x = atomic_load(&g_ui_x), .ui_y = atomic_load(&g_ui_y),
    .pointer_age_ms = atomic_load(&g_pointer_age_ms), .stale_events = atomic_load(&g_stale_events),
  };
}

static void queue_sample(touch_sample_t *sample, pet_p4_touch_decoder_t *gesture,
                         pet_p4_pointer_decoder_t *pointer) {
  if (!sample->has_pointer && !sample->has_gesture) return;
  if (xQueueSend(g_touch_queue, sample, 0) == pdTRUE) return;
  atomic_fetch_add(&g_dropped_events, 1);
  /* Unknown missing motion must never become a cut or a legacy swipe. */
  xQueueReset(g_touch_queue);
  sample->has_pointer = true; sample->has_gesture = false;
  sample->pointer.kind = PET_P4_POINTER_CANCEL;
  xQueueSend(g_touch_queue, sample, 0);
  memset(pointer, 0, sizeof(*pointer));
  pet_p4_touch_decoder_init(gesture, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
}

static uint64_t monotonic_ms(void) {
  return (uint64_t) (esp_timer_get_time() / 1000ULL);
}

static void IRAM_ATTR touch_interrupt_handler(void *arg) {
  BaseType_t task_awake = pdFALSE;
  vTaskNotifyGiveFromISR((TaskHandle_t)arg, &task_awake);
  if (task_awake == pdTRUE) portYIELD_FROM_ISR();
}

static esp_err_t init_touch_interrupt(void) {
  int interrupt_gpio = BSP_LCD_TOUCH_INT;
  const gpio_config_t config = {
    .pin_bit_mask = 1ULL << interrupt_gpio,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE,
  };
  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    gpio_reset_pin(interrupt_gpio);
    return err;
  }
  err = gpio_install_isr_service(0);
  bool own_service = err == ESP_OK;
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
  err = gpio_isr_handler_add(interrupt_gpio, touch_interrupt_handler, g_touch_task);
  if (err != ESP_OK) {
    if (own_service) gpio_uninstall_isr_service();
    goto fail;
  }
  // Consume a report whose IRQ pulse may have occurred during initialization.
  xTaskNotifyGive(g_touch_task);
  return ESP_OK;
fail:
  gpio_reset_pin(interrupt_gpio);
  return err;
}

static bool wait_for_touch_interrupt(pet_p4_touch_decoder_t *decoder,
                                     pet_p4_pointer_decoder_t *pointer) {
  TickType_t wait_ticks = portMAX_DELAY;
  if (decoder->down && !decoder->moved && !decoder->long_press_emitted) {
    uint64_t now = monotonic_ms();
    uint64_t deadline = decoder->start_ms + decoder->long_press_ms;
    wait_ticks = now < deadline ? pdMS_TO_TICKS(deadline - now) + 1 : 0;
  }
  // Clear before reading. IRQs arriving during I2C/ACK remain pending.
  if (ulTaskNotifyTake(pdTRUE, wait_ticks) != 0) return true;
  touch_sample_t sample = {0};
  sample.has_gesture = pet_p4_touch_decoder_update(decoder, decoder->down,
    decoder->current_x, decoder->current_y, monotonic_ms(), &sample.gesture);
  queue_sample(&sample, decoder, pointer);
  return false;
}

/* CST328 uses 16-bit registers and a 27-byte report, unlike CST826.
 * Register layout follows Hynitron CST328 datasheet v2.2, section 12.
 * Clear/sync sequence follows the Waveshare and ESPHome CST328 drivers. */
typedef struct {
  bool has_report;
  unsigned count;
  int x, y, track_id;
} cst328_frame_t;

static esp_err_t cst328_ack_report(void) {
  const uint8_t clear = 0, sync = 0xab;
  esp_err_t err = esp_lcd_panel_io_tx_param(g_touch_io, 0xd005, &clear, 1);
  esp_err_t sync_err = esp_lcd_panel_io_tx_param(g_touch_io, 0xd000, &sync, 1);
  return err != ESP_OK ? err : sync_err;
}

static esp_err_t read_cst328_touch(cst328_frame_t *frame) {
  uint8_t raw[27];
  memset(frame, 0, sizeof(*frame));
  esp_err_t err = esp_lcd_panel_io_rx_param(g_touch_io, 0xd000, raw, sizeof(raw));
  if (err != ESP_OK) return err;
  if (raw[6] != 0xab) return ESP_ERR_INVALID_RESPONSE;
  /* The consumed marker is not a lift. The upstream Hynitron CST3xx
   * driver also rejects D000 == AB before interpreting the finger count. */
  if (raw[0] == 0xab) return ESP_OK;
  esp_err_t ack_err = cst328_ack_report();
  if (ack_err != ESP_OK) return ack_err;
  unsigned count = raw[5] & 0x0f;
  if (count > 5 || (raw[5] & 0x80)) return ESP_ERR_INVALID_RESPONSE;
  frame->has_report = true;
  if (count == 0) return ESP_OK;
  /* Multi-contact reports cancel the stroke before any coordinates are used. */
  if (count > 1) {
    frame->count = count;
    return ESP_OK;
  }
  unsigned status = (raw[0] & 0x0f) >> 1;
  if (status == 0) return ESP_OK;  // Explicit lift report.
  if (status != 3) return ESP_ERR_INVALID_RESPONSE;
  frame->x = (raw[1] << 4) | (raw[3] >> 4);
  frame->y = (raw[2] << 4) | (raw[3] & 0x0f);
  if (frame->x >= BSP_LCD_H_RES || frame->y >= BSP_LCD_V_RES) return ESP_ERR_INVALID_RESPONSE;
  frame->count = 1;
  frame->track_id = raw[0] >> 4;
  return ESP_OK;
}

static esp_err_t identify_cst328_touch(void) {
  uint8_t check[4] = {0}, resolution[4] = {0}, identity[4] = {0};
  esp_err_t err = esp_lcd_panel_io_tx_param(g_touch_io, 0xd101, NULL, 0);
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(20));
    err = esp_lcd_panel_io_rx_param(g_touch_io, 0xd1fc, check, sizeof(check));
  }
  if (err == ESP_OK && (check[2] != 0xca || check[3] != 0xca)) err = ESP_ERR_INVALID_RESPONSE;
  if (err == ESP_OK) err = esp_lcd_panel_io_rx_param(g_touch_io, 0xd1f8, resolution, sizeof(resolution));
  if (err == ESP_OK && ((resolution[0] | (resolution[1] << 8)) != BSP_LCD_H_RES ||
                       (resolution[2] | (resolution[3] << 8)) != BSP_LCD_V_RES)) {
    ESP_LOGW(TAG, "CST328 resolution mismatch x=%u y=%u", resolution[0] | (resolution[1] << 8),
             resolution[2] | (resolution[3] << 8));
    err = ESP_ERR_NOT_SUPPORTED;
  }
  if (err == ESP_OK) err = esp_lcd_panel_io_rx_param(g_touch_io, 0xd204, identity, sizeof(identity));
  /* Always leave info mode, including after an I2C error or identity mismatch. */
  esp_err_t normal_err = esp_lcd_panel_io_tx_param(g_touch_io, 0xd109, NULL, 0);
  if (err != ESP_OK) return err;
  if (normal_err != ESP_OK) return normal_err;
  vTaskDelay(pdMS_TO_TICKS(20));
  err = cst328_ack_report();
  if (err != ESP_OK) return err;
  g_chip_id = identity[2] | (identity[3] << 8);
  g_cst328 = true;
  return ESP_OK;
}

static void touch_task(void *arg) {
  (void) arg;
  pet_p4_touch_decoder_t decoder;
  pet_p4_pointer_decoder_t pointer = {0};
  int track_id = -1;
  TickType_t last_wake = xTaskGetTickCount();
  unsigned int consecutive_errors = 0;
  unsigned int retry_attempts = 0;
  bool retry_pending = false;
  pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);

  while (true) {
    if (g_touch_interrupt_mode) {
      if (retry_pending) {
        vTaskDelay(pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
        retry_pending = false;
      } else {
        if (!wait_for_touch_interrupt(&decoder, &pointer)) continue;
        retry_attempts = 0;
      }
    }
    esp_err_t err = ESP_OK;
    bool pressed = false;
    bool contact_ambiguous = false;
    int ui_x = decoder.current_x;
    int ui_y = decoder.current_y;
    if (g_cst328) {
      cst328_frame_t frame;
      err = read_cst328_touch(&frame);
      if (err == ESP_OK && !frame.has_report) {
        if (!g_touch_interrupt_mode) {
          consecutive_errors = 0;
          atomic_store(&g_consecutive_errors, 0);
        }
        /* Keep a short gap between controller reports out of the gesture
         * decoder. Reuse the existing pointer freshness limit for a stall. */
        uint64_t now = monotonic_ms();
        if (!g_touch_interrupt_mode && pointer.down && now - pointer.last_ms > PET_P4_POINTER_STALE_MS) {
          touch_sample_t sample = {0};
          sample.has_pointer = pet_p4_pointer_update(&pointer, false, false, ui_x, ui_y, now, &sample.pointer);
          pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
          queue_sample(&sample, &decoder, &pointer);
        }
        if (!g_touch_interrupt_mode) vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
        continue;
      }
      if (err == ESP_OK) {
        contact_ambiguous = frame.count > 1;
        pressed = frame.count == 1;
        if (pressed) {
          contact_ambiguous = pointer.down && track_id != frame.track_id;
          track_id = frame.track_id;
          pet_p4_touch_panel_to_ui(frame.x, frame.y, &ui_x, &ui_y);
          atomic_store(&g_panel_x, frame.x); atomic_store(&g_panel_y, frame.y);
        }
      }
    } else if (g_ft6336 || g_cst826) {
      uint8_t raw[13]; pet_p4_ft6336_frame_t frame;
      err = esp_lcd_panel_io_rx_param(g_touch_io, 0x02, raw, sizeof(raw));
      if (err == ESP_OK && !pet_p4_ft6336_decode(raw, sizeof(raw), &frame)) err = ESP_ERR_INVALID_RESPONSE;
      if (err == ESP_OK) {
        contact_ambiguous = frame.count > 1;
        pressed = frame.count == 1;
        if (pressed) {
          contact_ambiguous = pointer.down && track_id != frame.track_id;
          track_id = frame.track_id;
          if (g_cst826) pet_p4_touch_cst826_to_ui(frame.x, frame.y, &ui_x, &ui_y);
          else pet_p4_touch_panel_to_ui(frame.x, frame.y, &ui_x, &ui_y);
          atomic_store(&g_panel_x, frame.x); atomic_store(&g_panel_y, frame.y);
        }
      }
    } else if ((err = esp_lcd_touch_read_data(g_touch)) == ESP_OK) {
      esp_lcd_touch_point_data_t points[2] = {0};
      uint8_t point_count = 0;
      err = esp_lcd_touch_get_data(g_touch, points, &point_count, 2);
      contact_ambiguous = point_count > 1;
      if (err == ESP_OK && point_count == 1) {
        pressed = true;
        contact_ambiguous = pointer.down && track_id != points[0].track_id;
        track_id = points[0].track_id;
        pet_p4_touch_panel_to_ui((int) points[0].x, (int) points[0].y, &ui_x, &ui_y);
        atomic_store(&g_panel_x, points[0].x); atomic_store(&g_panel_y, points[0].y);
      }
    }
    if (err != ESP_OK) {
      consecutive_errors += 1;
      atomic_fetch_add(&g_poll_errors, 1);
      atomic_store(&g_consecutive_errors, consecutive_errors);
      touch_sample_t sample = {0};
      sample.has_pointer = pet_p4_pointer_update(&pointer, false, false, ui_x, ui_y,
                                                monotonic_ms(), &sample.pointer);
      queue_sample(&sample, &decoder, &pointer);
      pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
      if (consecutive_errors == 1 || consecutive_errors % 200 == 0) {
        ESP_LOGW(TAG, "%s read failed count=%u error=%s",
                 pet_p4_touch_controller(), consecutive_errors, esp_err_to_name(err));
      }
      if (g_touch_interrupt_mode) {
        if (retry_attempts < 2) {
          retry_attempts += 1;
          retry_pending = true;
        } else {
          // Release a latched IRQ after the bounded retries, retaining the error status.
          (void)cst328_ack_report();
        }
      } else {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
      }
      continue;
    }
    consecutive_errors = 0;
    atomic_store(&g_consecutive_errors, 0);
    atomic_fetch_add(&g_samples, 1);
    if (pressed) {
      atomic_fetch_add(&g_contacts, 1);
      atomic_store(&g_ui_x, ui_x); atomic_store(&g_ui_y, ui_y);
    }
    touch_sample_t sample = {0};
    uint64_t now = monotonic_ms();
    // IRQ reports describe changes. A stationary finger may have no new IRQs.
    if (g_touch_interrupt_mode) pointer.last_ms = now;
    sample.has_pointer = pet_p4_pointer_update(&pointer, !contact_ambiguous, pressed, ui_x, ui_y, now, &sample.pointer);
    if (contact_ambiguous) {
      pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
      queue_sample(&sample, &decoder, &pointer);
      if (!g_touch_interrupt_mode) vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
      continue;
    }
    if (sample.has_pointer && sample.pointer.kind == PET_P4_POINTER_CANCEL)
      pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
    else {
#if PET_P4_SYSTEM_WIFI
      if (pressed && !decoder.down) {
        const int context = atomic_load_explicit(&g_touch_context, memory_order_relaxed);
        decoder.anchor_tap = context == TOUCH_SETTINGS_PAGE ||
          (context == TOUCH_SETTINGS_ENTRY && pet_center_settings_hit(ui_x, ui_y));
      }
#endif
      sample.has_gesture = pet_p4_touch_decoder_update(&decoder, pressed, ui_x, ui_y, now, &sample.gesture);
    }
    queue_sample(&sample, &decoder, &pointer);
    if (!g_touch_interrupt_mode) vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
  }
}

/* CST826 uses CHIPTYPE at 0xAA (0x11), not FT6336U's 0xA3 (0x64).
 * Register facts: Adafruit CST8XX and Espressif FT6336U documentation.
 * Both use count at 0x02 and six-byte point records; only one contact is
 * dispatched. Identification is read-only, with no GPIO/config writes. */
static esp_err_t identify_register_touch(void) {
  uint8_t cst_id = 0, ft_id = 0;
  esp_err_t err = esp_lcd_panel_io_rx_param(g_touch_io, 0xaa, &cst_id, 1);
  if (err == ESP_OK && cst_id == 0x11) {
    g_cst826 = true;
    g_chip_id = cst_id;
  } else {
    err = esp_lcd_panel_io_rx_param(g_touch_io, 0xa3, &ft_id, 1);
    if (err != ESP_OK) return err;
    g_chip_id = ft_id;
    if (ft_id != 0x64) {
      ESP_LOGW(TAG, "unknown touch addr=0x%02x CST ID=0x%02x FT ID=0x%02x", g_address, cst_id, ft_id);
      return ESP_ERR_NOT_SUPPORTED;
    }
    g_ft6336 = true;
  }
  /* Module/vendor ID is diagnostic only; it is not an identity gate. */
  (void)esp_lcd_panel_io_rx_param(g_touch_io, 0xa8, &g_vendor_id, 1);
  uint8_t raw[13]; pet_p4_ft6336_frame_t frame;
  err = esp_lcd_panel_io_rx_param(g_touch_io, 0x02, raw, sizeof(raw));
  if (err == ESP_OK && !pet_p4_ft6336_decode(raw, sizeof(raw), &frame)) err = ESP_ERR_INVALID_RESPONSE;
  if (err != ESP_OK) g_cst826 = g_ft6336 = false;
  return err;
}

#if PET_P4_COLD_INIT_SIZE && defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("Oz")))
#endif
esp_err_t pet_p4_touch_init(void) {
  if (g_ready) return ESP_OK;
  g_address = 0;
  g_cst328 = g_cst826 = g_ft6336 = false;
  g_touch_interrupt_mode = false;
  g_chip_id = g_vendor_id = 0;
  g_init_stage = "power_on_wait";
  /* The adapter uses RC resets. Also allow direct touch-only initialization. */
  uint64_t uptime_ms = monotonic_ms();
  if (uptime_ms < BSP_LCD_POWER_ON_DELAY_MS) {
    vTaskDelay(pdMS_TO_TICKS(BSP_LCD_POWER_ON_DELAY_MS - uptime_ms) + 1);
  }
  g_init_stage = "i2c_init";
  esp_err_t err = bsp_i2c_init();
  if (err != ESP_OK) { g_init_error = err; return err; }
  i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
  if (!i2c_bus) { g_init_error = ESP_ERR_INVALID_STATE; return g_init_error; }

  esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  g_init_stage = "probe";
  g_primary_error = g_backup_error = ESP_ERR_NOT_FOUND;
  /* Probe only. Both resets are generated by the adapter RC circuits. */
  for (unsigned attempt = 0; attempt < 4 && !g_address; attempt++) {
    g_probe_1a = i2c_master_probe(i2c_bus, PET_P4_CST328_ADDRESS, 50);
    if (g_probe_1a == ESP_OK) {
      g_address = PET_P4_CST328_ADDRESS;
      break;
    }
    g_primary_error = i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 50);
    if (g_primary_error == ESP_OK) g_address = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    else {
      g_backup_error = i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 50);
      if (g_backup_error == ESP_OK) g_address = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    }
    if (!g_address && attempt < 3) vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (g_address == PET_P4_CST328_ADDRESS) {
    io_config = (esp_lcd_panel_io_i2c_config_t){
      .dev_addr = PET_P4_CST328_ADDRESS, .scl_speed_hz = 100000,
      .control_phase_bytes = 1, .lcd_cmd_bits = 16, .flags.disable_control_phase = 1,
    };
    g_init_stage = "identify_cst328";
    err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &g_touch_io);
    if (err == ESP_OK) err = identify_cst328_touch();
    if (err != ESP_OK) {
      if (g_touch_io) esp_lcd_panel_io_del(g_touch_io);
      g_touch_io = NULL;
      g_init_error = err;
      return err;
    }
  }
  if (!g_address) {
    /* Address ACKs only, not controller identification or register writes.
     * Distinguish missing GT911 from an entirely unreachable shared bus. */
    g_probe_38 = i2c_master_probe(i2c_bus, 0x38, 50);
    g_probe_15 = i2c_master_probe(i2c_bus, 0x15, 50);
    g_probe_10 = i2c_master_probe(i2c_bus, 0x10, 50);
    g_probe_18 = i2c_master_probe(i2c_bus, 0x18, 50);
    g_init_error = (g_primary_error == ESP_ERR_TIMEOUT || g_backup_error == ESP_ERR_TIMEOUT)
      ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
    ESP_LOGW(TAG, "GT911 probe failed SDA=%d SCL=%d 0x5d=%s 0x14=%s; reset not driven",
      BSP_I2C_SDA, BSP_I2C_SCL, esp_err_to_name(g_primary_error), esp_err_to_name(g_backup_error));
    if (g_probe_38 != ESP_OK && g_probe_15 != ESP_OK) return g_init_error;
    io_config = (esp_lcd_panel_io_i2c_config_t){
      .dev_addr = 0x38, .scl_speed_hz = 100000, .control_phase_bytes = 1,
      .lcd_cmd_bits = 8, .flags.disable_control_phase = 1,
    };
    const int addresses[] = {0x15, 0x38};
    for (unsigned i = 0; i < 2; ++i) {
      if ((addresses[i] == 0x15 ? g_probe_15 : g_probe_38) != ESP_OK) continue;
      g_address = io_config.dev_addr = addresses[i];
      g_init_stage = "identify_register_touch";
      err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &g_touch_io);
      if (err == ESP_OK) err = identify_register_touch();
      if (err == ESP_OK) break;
      if (g_touch_io) esp_lcd_panel_io_del(g_touch_io);
      g_touch_io = NULL;
    }
    if (!g_touch_io) { g_init_error = err; return err; }
  }
  io_config.dev_addr = g_address;
  g_init_stage = "panel_io";
  io_config.scl_speed_hz = (g_cst328 || g_ft6336 || g_cst826) ? 100000 : CONFIG_BSP_I2C_CLK_SPEED_HZ;
  if (!g_touch_io) err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &g_touch_io);
  if (err != ESP_OK) { g_init_error = err; return err; }

  const esp_lcd_touch_config_t touch_config = {
    .x_max = BSP_LCD_H_RES,
    .y_max = BSP_LCD_V_RES,
    .rst_gpio_num = GPIO_NUM_NC,
    .int_gpio_num = GPIO_NUM_NC,
    .levels = {
      .reset = 0,
      .interrupt = 0,
    },
    .flags = {
      .swap_xy = 0,
      .mirror_x = 0,
      .mirror_y = 0,
    },
  };
  g_init_stage = "controller";
  if (!g_cst328 && !g_ft6336 && !g_cst826) err = esp_lcd_touch_new_i2c_gt911(g_touch_io, &touch_config, &g_touch);
  if (err != ESP_OK) {
    esp_lcd_panel_io_del(g_touch_io);
    g_touch_io = NULL;
    g_init_error = err; return err;
  }

  g_init_stage = "queue";
  g_touch_queue = xQueueCreate(PET_P4_TOUCH_QUEUE_LENGTH, sizeof(touch_sample_t));
  if (!g_touch_queue) { err = ESP_ERR_NO_MEM; goto task_fail; }
  g_touch_interrupt_mode = g_cst328 && BSP_LCD_TOUCH_INT >= 0;
  g_init_stage = "task";
  if (xTaskCreate(touch_task, "pet_p4_touch", 4096, NULL, 8, &g_touch_task) != pdPASS) {
    err = ESP_ERR_NO_MEM;
    goto task_fail;
  }
  if (g_touch_interrupt_mode) {
    g_init_stage = "interrupt";
    err = init_touch_interrupt();
    if (err != ESP_OK) goto task_fail;
  }
  g_ready = true;
  g_init_stage = "ready"; g_init_error = ESP_OK;
  ESP_LOGI(TAG, "%s ready addr=0x%02x chip=0x%02x mode=%s int_gpio=%d",
           pet_p4_touch_controller(), io_config.dev_addr, g_chip_id,
           g_touch_interrupt_mode ? "interrupt" : "polling",
           g_touch_interrupt_mode ? BSP_LCD_TOUCH_INT : GPIO_NUM_NC);
  return ESP_OK;
task_fail:
  if (g_touch_task) vTaskDelete(g_touch_task);
  g_touch_task = NULL;
  if (g_touch_queue) vQueueDelete(g_touch_queue);
  g_touch_queue = NULL;
  g_touch_interrupt_mode = false;
  if (g_touch) esp_lcd_touch_del(g_touch);
  g_touch = NULL; g_cst328 = g_cst826 = g_ft6336 = false;
  esp_lcd_panel_io_del(g_touch_io); g_touch_io = NULL;
  g_init_error = err;
  return g_init_error;
}

bool pet_p4_touch_ready(void) {
  return g_ready && atomic_load(&g_consecutive_errors) < 3;
}

static const char *touch_event_name(pet_p4_touch_gesture_t gesture) {
  if (gesture == PET_P4_TOUCH_TAP) return "screen.region.tap";
  if (gesture == PET_P4_TOUCH_LONG_PRESS) return "screen.region.long_press";
  if (gesture == PET_P4_TOUCH_SWIPE_LEFT) return "touch.swipe_left";
  if (gesture == PET_P4_TOUCH_SWIPE_RIGHT) return "touch.swipe_right";
  if (gesture == PET_P4_TOUCH_SWIPE_UP) return "touch.swipe_up";
  if (gesture == PET_P4_TOUCH_SWIPE_DOWN) return "touch.swipe_down";
  return "touch.unknown";
}

static bool gesture_is_swipe(pet_p4_touch_gesture_t gesture) {
  return gesture == PET_P4_TOUCH_SWIPE_LEFT
    || gesture == PET_P4_TOUCH_SWIPE_RIGHT
    || gesture == PET_P4_TOUCH_SWIPE_UP
    || gesture == PET_P4_TOUCH_SWIPE_DOWN;
}

static const char *choose_touch_family(
  const pet_p4_runtime_state_t *state,
  int x,
  char *family,
  size_t family_size
) {
  const pet_p4_asset_catalog_t *catalog = state ? &state->asset_catalog : NULL;
  const char *directional = x < 214 ? "touch.left" : (x > 426 ? "touch.right" : "");
  if (directional[0] && catalog && pet_p4_asset_catalog_find_exact(catalog, directional) >= 0) {
    snprintf(family, family_size, "%s", directional);
    return family;
  }
  const char *first = (g_reaction_sequence++ % 2U) == 0 ? "touch.lick" : "touch.what";
  const char *second = strcmp(first, "touch.lick") == 0 ? "touch.what" : "touch.lick";
  if (catalog && pet_p4_asset_catalog_find_exact(catalog, first) >= 0) {
    snprintf(family, family_size, "%s", first);
  } else if (catalog && pet_p4_asset_catalog_find_exact(catalog, second) >= 0) {
    snprintf(family, family_size, "%s", second);
  } else {
    snprintf(family, family_size, "%s", first);
  }
  return family;
}

static uint64_t touch_reaction_duration_ms(
  const pet_p4_runtime_state_t *state,
  const char *family
) {
  const pet_p4_asset_catalog_t *catalog = state ? &state->asset_catalog : NULL;
  int index = catalog ? pet_p4_asset_catalog_find_exact(catalog, family) : -1;
  if (index < 0 && catalog) index = pet_p4_asset_catalog_nth_prefix(catalog, "touch.", 0);
  uint64_t duration = PET_P4_TOUCH_REACTION_MIN_MS;
  if (catalog && index >= 0 && index < catalog->count) {
    const pet_p4_asset_entry_t *entry = &catalog->entries[index];
    if (entry->frame_duration_ms > 0) {
      duration = (uint64_t) entry->frames * entry->frame_duration_ms;
    } else if (entry->fps > 0) {
      duration = ((uint64_t) entry->frames * 1000ULL) / (uint64_t) entry->fps;
    }
  }
  if (duration < PET_P4_TOUCH_REACTION_MIN_MS) duration = PET_P4_TOUCH_REACTION_MIN_MS;
  return duration;
}

static void send_touch_event(
  const pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_touch_event_t *event,
  const char *action,
  bool handled_locally
) {
  if (!send_line || !event) return;
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", "input/event");
  cJSON_AddNumberToObject(payload, "version", 1);
  cJSON_AddNumberToObject(payload, "seq", ++g_event_sequence);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddStringToObject(payload, "control", "screen.touch");
  cJSON_AddStringToObject(payload, "gesture", pet_p4_touch_gesture_name(event->gesture));
  cJSON_AddStringToObject(payload, "event", touch_event_name(event->gesture));
  cJSON_AddStringToObject(payload, "context", state ? state->screen_page : "main");
  cJSON_AddStringToObject(payload, "action", action ? action : "disabled");
  cJSON_AddNumberToObject(payload, "x", event->x);
  cJSON_AddNumberToObject(payload, "y", event->y);
  cJSON_AddNumberToObject(payload, "dx", event->dx);
  cJSON_AddNumberToObject(payload, "dy", event->dy);
  cJSON_AddNumberToObject(payload, "durationMs", (double) event->duration_ms);
  cJSON_AddBoolToObject(payload, "handledLocally", handled_locally);
  cJSON_AddNumberToObject(payload, "tsMs", (double) event->ts_ms);
  cJSON_AddNumberToObject(
    payload,
    "dropped",
    atomic_load_explicit(&g_dropped_events, memory_order_relaxed)
  );
  cJSON_AddItemToObject(root, "payload", payload);
  char *line = cJSON_PrintUnformatted(root);
  if (line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static void process_touch_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_touch_event_t *event
) {
  if (pet_p4_conversation_active(state)) {
    if (event->gesture == PET_P4_TOUCH_SWIPE_LEFT || event->gesture == PET_P4_TOUCH_SWIPE_UP)
      pet_p4_conversation_move(state, -1, event->ts_ms);
    else if (event->gesture == PET_P4_TOUCH_SWIPE_RIGHT || event->gesture == PET_P4_TOUCH_SWIPE_DOWN)
      pet_p4_conversation_move(state, 1, event->ts_ms);
    return;
  }
#if PET_P4_SYSTEM_WIFI
  if (pet_settings_active(state)) {
    if (event->gesture == PET_P4_TOUCH_TAP)
      pet_settings_touch(state, event->x, event->y, event->ts_ms);
    else if (event->gesture == PET_P4_TOUCH_SWIPE_UP || event->gesture == PET_P4_TOUCH_SWIPE_DOWN)
      pet_settings_wifi_swipe(state, event->x - event->dx, event->y - event->dy,
                              event->gesture == PET_P4_TOUCH_SWIPE_UP, event->ts_ms);
    return;
  }
#endif
  bool handled_locally = false;
  char action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  if(state&&!strcmp(state->screen_page,"components")) {
#if PET_P4_SYSTEM_WIFI
    if (event->gesture == PET_P4_TOUCH_TAP && pet_center_settings_hit(event->x, event->y) &&
        pet_p4_is_wifi6_hardware()) {
      (void)pet_settings_open(state, event->ts_ms);
      return;
    }
#endif
    const size_t count=pet_p4_miniapp_center_count();
    const size_t selected=pet_p4_miniapp_center_selected();
    if(event->gesture==PET_P4_TOUCH_TAP) {
      int tab=pet_center_tab_hit(event->x,event->y);
      int hit=pet_center_hit(event->x,event->y,selected,count);
      if(tab>=0) {
        handled_locally=pet_p4_miniapp_center_set_filter((pet_center_filter_t)tab);
        snprintf(action,sizeof(action),"component_filter");
      } else if(hit>=0&&pet_p4_miniapp_center_select((size_t)hit)) {
        pet_p4_miniapp_activation_request_t activation=pet_p4_miniapp_activation_request_selected();
        if(activation!=PET_MINIAPP_ACTIVATION_REJECTED) {
          if(activation==PET_MINIAPP_ACTIVATION_READY) {
            pet_p4_miniapp_sync_stats(&state->stats);
            snprintf(state->screen_page,sizeof(state->screen_page),"app");
          }
          snprintf(action,sizeof(action),"%s",activation==PET_MINIAPP_ACTIVATION_READY?"component_open":"component_open_pending");handled_locally=true;
        }
      }
    }else if(event->gesture==PET_P4_TOUCH_SWIPE_LEFT||event->gesture==PET_P4_TOUCH_SWIPE_RIGHT) {
      size_t next=pet_center_page(selected,count,event->gesture==PET_P4_TOUCH_SWIPE_LEFT?1:-1);
      handled_locally=count>0&&pet_p4_miniapp_center_select(next);
      snprintf(action,sizeof(action),"component_select");
    }
    if(handled_locally)state->last_update_ms+=1;
    send_touch_event(state,send_line,ctx,event,action,handled_locally);
    return;
  }
  if (gesture_is_swipe(event->gesture)) {
    const bool main_open = state && strcmp(state->screen_page, "main") == 0;
    const char *next_page = main_open ? "components"
      : state && strcmp(state->screen_page, "app") == 0 ? "components"
      : "main";
    if (state) {
      if (main_open) pet_p4_miniapp_catalog_focus_active();
      snprintf(state->screen_page, sizeof(state->screen_page), "%s", next_page);
      state->last_update_ms += 1;
      handled_locally = true;
      snprintf(action, sizeof(action), "page_%s", next_page);
    }
  } else {
    const char *event_name = touch_event_name(event->gesture);
    if (state && strcmp(state->screen_page, "app") == 0) {
      if(event->gesture==PET_P4_TOUCH_TAP&&pet_p4_miniapp_instrument_config(NULL,NULL)){
        snprintf(action,sizeof(action),"instrument.strike");
        handled_locally=pet_p4_miniapp_dispatch_action(action,event->ts_ms);
      }else handled_locally = pet_p4_miniapp_dispatch_input(
        event_name,
        event->ts_ms,
        action,
        sizeof(action)
      );
      if (handled_locally) state->last_update_ms += 1;
    }
    if (!handled_locally && state && strcmp(state->screen_page, "main") == 0) {
      char family[PET_P4_STATE_MAX];
      choose_touch_family(state, event->x, family, sizeof(family));
      handled_locally = pet_p4_state_request_touch(
        state,
        family,
        event->x,
        event->y,
        touch_reaction_duration_ms(state, family),
        event->ts_ms
      );
      if (handled_locally) snprintf(action, sizeof(action), "pet_touch");
    }
  }
  send_touch_event(state, send_line, ctx, event, action, handled_locally);
}

void pet_p4_touch_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  touch_sample_t sample;
  if (!g_touch_queue) return;
#if PET_P4_SYSTEM_WIFI
  int context = TOUCH_GENERAL;
  if (state && !pet_p4_conversation_active(state)) {
    if (pet_settings_active(state)) context = TOUCH_SETTINGS_PAGE;
    else if (!strcmp(state->screen_page, "components") && pet_p4_is_wifi6_hardware())
      context = TOUCH_SETTINGS_ENTRY;
  }
  atomic_store_explicit(&g_touch_context, context, memory_order_relaxed);
#endif
  while (xQueueReceive(g_touch_queue, &sample, 0) == pdTRUE) {
    bool test = state && !pet_p4_conversation_active(state) && !strcmp(state->screen_page, "touch_test");
    bool component = state && !pet_p4_conversation_active(state) && !strcmp(state->screen_page,"app")
      && pet_p4_miniapp_slicing_active();
    bool application = state && !pet_p4_conversation_active(state) && !strcmp(state->screen_page,"app")
      && pet_p4_miniapp_pointer_active();
#if PET_P4_SYSTEM_WIFI
    if (sample.has_pointer && sample.pointer.kind == PET_P4_POINTER_DOWN) {
      g_account_stroke = state && !strcmp(state->screen_page, "app") &&
        pet_account_button_hit(sample.pointer.x, sample.pointer.y) &&
        !pet_p4_conversation_active(state) && !pet_p4_miniapp_activation_pending() &&
        pet_p4_miniapp_private_feed(NULL, NULL);
    }
    if (g_account_stroke) {
      if (sample.has_gesture && sample.gesture.gesture == PET_P4_TOUCH_TAP &&
          pet_account_button_hit(sample.gesture.x, sample.gesture.y) &&
          state && !strcmp(state->screen_page, "app") && !pet_p4_conversation_active(state) &&
          !pet_p4_miniapp_activation_pending()) {
        (void)pet_settings_open(state, sample.gesture.ts_ms);
      }
      if (sample.has_pointer && (sample.pointer.kind == PET_P4_POINTER_UP ||
                                 sample.pointer.kind == PET_P4_POINTER_CANCEL)) g_account_stroke = false;
      continue;
    }
#endif
    // Timer-driven long presses can arrive without a new pointer report.
    bool captured = g_test_stroke || g_component_stroke || g_app_stroke;
    if (sample.has_pointer) {
      if (sample.pointer.kind == PET_P4_POINTER_DOWN) g_test_stroke = test;
      if (sample.pointer.kind == PET_P4_POINTER_DOWN) g_component_stroke = component;
      if (sample.pointer.kind == PET_P4_POINTER_DOWN) g_app_stroke = application;
      captured = g_test_stroke;
      if(g_component_stroke) {
        if(!component || monotonic_ms()-sample.pointer.ts_ms>150)sample.pointer.kind=PET_P4_POINTER_CANCEL;
        pet_p4_miniapp_pointer(&sample.pointer);
        captured=true;
      }
      if(g_app_stroke) {
        pet_p4_pointer_event_t event=sample.pointer;
        if(!application || monotonic_ms()-event.ts_ms>150)event.kind=PET_P4_POINTER_CANCEL;
        pet_p4_miniapp_pointer(&event);
        captured=true;
      }
      if (g_test_stroke && test) {
        /* Discard stale queued input; never join it to a fresh stroke. */
        uint64_t age = monotonic_ms() - sample.pointer.ts_ms;
        atomic_store(&g_pointer_age_ms, (unsigned)age);
        if (age > 150) {
          sample.pointer.kind = PET_P4_POINTER_CANCEL;
          atomic_fetch_add(&g_stale_events, 1);
        }
        pet_p4_slice_tick(&state->touch_test, sample.pointer.ts_ms);
        pet_p4_slice_pointer(&state->touch_test, &sample.pointer);
      }
      if (sample.pointer.kind == PET_P4_POINTER_UP || sample.pointer.kind == PET_P4_POINTER_CANCEL)
        g_test_stroke = g_component_stroke = g_app_stroke = false;
    }
    /* An application owns the entire stroke, including swipes and cancellation.
       Never reinterpret an in-app drag as global navigation. */
    if (sample.has_gesture && !captured && !test && !component
        && !application)
      process_touch_event(state, send_line, ctx, &sample.gesture);
    pet_p4_miniapp_activation_guard(state && !strcmp(state->screen_page, "components")
      && !pet_p4_conversation_active(state));
  }
}

unsigned int pet_p4_touch_dropped_events(void) {
  return atomic_load_explicit(&g_dropped_events, memory_order_relaxed);
}
