"""Exercise production LCD visibility and fixed-backlight handling on the host."""
from pathlib import Path
import os
import shutil
import subprocess
from p4_render_trace_test import function
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_lcd_adapter_visibility():
    lcd = (ROOT / "main/pet_p4_lcd.c").read_text(encoding="utf-8")
    bsp = (ROOT / "components/esp32_p4_wifi6_touch_lcd_4_3/"
           "esp32_p4_wifi6_touch_lcd_4_3.c").read_text(encoding="utf-8")
    brightness = bsp.split("esp_err_t bsp_display_brightness_init(void)", 1)[1]
    brightness = "esp_err_t bsp_display_brightness_init(void)" + brightness.split(
        "static esp_err_t bsp_enable_dsi_phy_power", 1)[0]
    visibility = lcd.split("static esp_err_t force_lcd_backlight_gpio", 1)[1]
    visibility = "static esp_err_t force_lcd_backlight_gpio" + visibility.split(
        "static bool IRAM_ATTR lcd_refresh_done_callback", 1)[0]
    reveal = (
        function(lcd, "pet_p4_lcd_reveal")
        + function(lcd, "pet_p4_lcd_backlight_status")
        + function(lcd, "pet_p4_lcd_keep_awake")
    )
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_SUPPORTED 2
#define IO_ERROR 3
#define GPIO_ERROR 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(call, ...) do { \
  esp_err_t result = (call); if (result != ESP_OK) return result; \
} while (0)
#define BSP_ERROR_CHECK_RETURN_ERR(call) ESP_RETURN_ON_ERROR(call)
enum { GPIO_MODE_OUTPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE,
       GPIO_INTR_DISABLE, LEDC_LOW_SPEED_MODE, LEDC_INTR_DISABLE,
       LEDC_TIMER_10_BIT, LEDC_AUTO_CLK, LCD_LEDC_CH };
static int test_backlight_gpio;
#define BSP_LCD_BACKLIGHT test_backlight_gpio
typedef struct {
  uint64_t pin_bit_mask;
  int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
typedef struct {
  int gpio_num, speed_mode, channel, intr_type, timer_sel, duty, hpoint;
  struct { int output_invert; } flags;
} ledc_channel_config_t;
typedef struct {
  int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg, deconfigure;
} ledc_timer_config_t;
static uint8_t brightness;
static const int LCD_BACKLIGHT_FORCE_LEVEL = 1;
static void *g_panel_handle;
static esp_err_t g_backlight_status;
static bool g_backlight_revealed, g_forced_backlight_logged;
static int gpio_configs, gpio_levels, ledc_calls, panel_calls;
static int gpio_config_error, gpio_level_error, ledc_error, panel_error;
static uint64_t last_gpio_mask;
static int last_gpio_level, last_duty;
static bool last_visible;
static esp_err_t gpio_config(const gpio_config_t *config) {
  gpio_configs++;
  last_gpio_mask = config->pin_bit_mask;
  return gpio_config_error;
}
static esp_err_t gpio_set_level(int gpio, int level) {
  assert(gpio >= 0 && gpio == test_backlight_gpio);
  gpio_levels++;
  last_gpio_level = level;
  return gpio_level_error;
}
static esp_err_t ledc_timer_config(const ledc_timer_config_t *config) {
  assert(config->timer_num == 1);
  ledc_calls++;
  return ledc_error;
}
static esp_err_t ledc_channel_config(const ledc_channel_config_t *config) {
  assert(config->gpio_num >= 0 && config->gpio_num == test_backlight_gpio);
  ledc_calls++;
  return ledc_error;
}
static esp_err_t ledc_timer_pause(int mode, int timer) {
  assert(mode == LEDC_LOW_SPEED_MODE && timer == 1);
  ledc_calls++;
  return ledc_error;
}
static esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty) {
  assert(mode == LEDC_LOW_SPEED_MODE && channel == LCD_LEDC_CH);
  ledc_calls++;
  last_duty = (int)duty;
  return ledc_error;
}
static esp_err_t ledc_update_duty(int mode, int channel) {
  assert(mode == LEDC_LOW_SPEED_MODE && channel == LCD_LEDC_CH);
  ledc_calls++;
  return ledc_error;
}
static esp_err_t esp_lcd_panel_disp_on_off(void *panel, bool visible) {
  assert(panel != NULL && panel == g_panel_handle);
  panel_calls++;
  last_visible = visible;
  return panel_error;
}
'''
    harness += brightness + visibility + reveal + r'''
static void reset(int gpio) {
  test_backlight_gpio = gpio;
  g_panel_handle = NULL;
  g_backlight_revealed = g_forced_backlight_logged = false;
  g_backlight_status = ESP_ERR_INVALID_STATE;
  gpio_configs = gpio_levels = ledc_calls = panel_calls = 0;
  gpio_config_error = gpio_level_error = ledc_error = panel_error = ESP_OK;
  last_gpio_mask = 0;
  last_gpio_level = last_duty = -1;
}
int main(void) {
  reset(-1);
  assert(bsp_display_brightness_init() == ESP_OK);
  assert(bsp_display_brightness_get() == 100);
  assert(bsp_display_brightness_deinit() == ESP_OK);
  assert(bsp_display_brightness_set(100) == ESP_OK);
  assert(bsp_display_brightness_set(50) == ESP_ERR_NOT_SUPPORTED);
  assert(bsp_display_backlight_off() == ESP_ERR_NOT_SUPPORTED);
  assert(bsp_display_backlight_on() == ESP_OK);
  assert(force_lcd_backlight_gpio(1) == ESP_OK);
  assert(pet_p4_lcd_prepare_boot() == ESP_OK);
  assert(!g_backlight_revealed);
  assert(pet_p4_lcd_reveal() == ESP_ERR_INVALID_STATE);
  assert(pet_p4_lcd_keep_awake() == ESP_ERR_INVALID_STATE);
  assert(panel_calls == 0 && gpio_configs == 0 && gpio_levels == 0 && ledc_calls == 0);

  g_panel_handle = &brightness;
  assert(set_lcd_visible(false) == ESP_OK);
  assert(panel_calls == 1 && !last_visible);
  panel_error = IO_ERROR;
  assert(set_lcd_visible(false) == IO_ERROR);
  assert(pet_p4_lcd_reveal() == IO_ERROR);
  assert(!g_backlight_revealed && pet_p4_lcd_backlight_status() == IO_ERROR);
  assert(pet_p4_lcd_keep_awake() == ESP_ERR_INVALID_STATE);
  panel_error = ESP_OK;
  assert(pet_p4_lcd_reveal() == ESP_OK);
  assert(g_backlight_revealed && last_visible && panel_calls == 4);
  assert(pet_p4_lcd_reveal() == ESP_OK && panel_calls == 4);
  assert(pet_p4_lcd_keep_awake() == ESP_OK);
  assert(gpio_configs == 0 && gpio_levels == 0 && ledc_calls == 0);

  reset(26);
  assert(bsp_display_brightness_init() == ESP_OK && ledc_calls == 2);
  assert(bsp_display_brightness_deinit() == ESP_OK && ledc_calls == 4);
  assert(pet_p4_lcd_prepare_boot() == ESP_OK);
  assert(gpio_configs == 1 && gpio_levels == 1 && last_gpio_level == 0);
  assert(last_gpio_mask == (1ULL << 26));
  g_panel_handle = &brightness;
  assert(set_lcd_visible(false) == ESP_OK);
  assert(last_gpio_level == 0 && last_duty == 0 && brightness == 0);
  assert(pet_p4_lcd_reveal() == ESP_OK);
  assert(last_gpio_level == 1 && last_duty == 1023 && brightness == 100);
  assert(g_backlight_revealed && panel_calls == 0);
  int calls_after_reveal = gpio_configs + gpio_levels + ledc_calls;
  assert(pet_p4_lcd_reveal() == ESP_OK);
  assert(calls_after_reveal == gpio_configs + gpio_levels + ledc_calls);

  reset(26);
  g_panel_handle = &brightness;
  ledc_error = IO_ERROR;
  gpio_config_error = GPIO_ERROR;
  assert(pet_p4_lcd_reveal() == IO_ERROR);
  assert(!g_backlight_revealed && gpio_configs == 1 && gpio_levels == 0);
  ledc_error = ESP_OK;
  assert(pet_p4_lcd_reveal() == GPIO_ERROR && !g_backlight_revealed);
  gpio_config_error = ESP_OK;
  gpio_level_error = GPIO_ERROR;
  assert(pet_p4_lcd_reveal() == GPIO_ERROR && !g_backlight_revealed);
  gpio_level_error = ESP_OK;
  assert(pet_p4_lcd_reveal() == ESP_OK && g_backlight_revealed);
  assert(panel_calls == 0);
}
'''
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    assert compiler, "A host C compiler is required"
    with tempfile.TemporaryDirectory(prefix="p4-lcd-adapter-") as directory:
        path = Path(directory) / "lcd_adapter.c"
        path.write_text(harness, encoding="utf-8")
        binary = path.with_suffix(".exe" if os.name == "nt" else "")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
        if os.name != "nt":
            flags.append("-fsanitize=address,undefined")
        subprocess.run([compiler, *flags, str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        # Match the firmware's optimization with a constant NC pin to catch
        # negative-shift diagnostics raised after the syntax-checking phase.
        nc_path = Path(directory) / "lcd_adapter_nc.c"
        nc_path.write_text(harness.replace(
            "#define BSP_LCD_BACKLIGHT test_backlight_gpio",
            "#define BSP_LCD_BACKLIGHT (-1)"), encoding="utf-8")
        subprocess.run([compiler, *flags, "-O2", "-c", str(nc_path),
                        "-o", str(nc_path.with_suffix(".o"))], check=True)
