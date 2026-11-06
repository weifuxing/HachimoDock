"""Run the production CST328 register transactions against an I2C fake."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_cst328_register_protocol(tmp_path):
    source = (ROOT / "main/pet_p4_touch.c").read_text(encoding="utf-8")
    helpers = source.split("/* CST328 uses 16-bit registers", 1)[1]
    helpers = "/* CST328 uses 16-bit registers" + helpers.split("static void touch_task", 1)[0]
    idle_branch = source.split("      if (err == ESP_OK && !frame.has_report) {", 1)[1]
    idle_branch = "if (err == ESP_OK && !frame.has_report) {" + idle_branch.split("      if (err == ESP_OK) {", 1)[0]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include "pet_p4_touch_core.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE 1
#define ESP_ERR_NOT_SUPPORTED 2
#define BUS_ERROR 3
#define BSP_LCD_H_RES 480
#define BSP_LCD_V_RES 640
#define PET_P4_TOUCH_SAMPLE_MS 15
#define PET_P4_TOUCH_SWIPE_THRESHOLD 48
#define PET_P4_TOUCH_LONG_PRESS_MS 700ULL
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ticks) { (void)ticks; }
static void vTaskDelayUntil(int *wake, int ticks) { (void)wake; (void)ticks; }
typedef struct {
  bool has_pointer, has_gesture;
  pet_p4_pointer_event_t pointer;
  pet_p4_touch_event_t gesture;
} touch_sample_t;
static touch_sample_t queued;
static atomic_uint g_consecutive_errors;
static uint64_t clock_ms;
static uint64_t monotonic_ms(void) { return clock_ms; }
static void queue_sample(touch_sample_t *sample, pet_p4_touch_decoder_t *gesture,
                         pet_p4_pointer_decoder_t *pointer) {
  (void)gesture; (void)pointer; queued = *sample;
}
static void *g_touch_io;
static uint16_t g_chip_id;
static bool g_cst328;
static bool g_touch_interrupt_mode;
static uint8_t report[27], check[4], resolution[4], identity[4];
static int fail_read, fail_write, written[8], write_count;
static int esp_lcd_panel_io_rx_param(void *io, int reg, void *out, size_t n) {
  (void)io;
  if (reg == fail_read) return BUS_ERROR;
  const uint8_t *data = NULL;
  switch (reg) {
    case 0xd000: assert(n == sizeof(report)); data = report; break;
    case 0xd1fc: assert(n == sizeof(check)); data = check; break;
    case 0xd1f8: assert(n == sizeof(resolution)); data = resolution; break;
    case 0xd204: assert(n == sizeof(identity)); data = identity; break;
    default: assert(false);
  }
  memcpy(out, data, n);
  return ESP_OK;
}
static int esp_lcd_panel_io_tx_param(void *io, int reg, const void *data, size_t n) {
  (void)io;
  assert(write_count < 8);
  written[write_count++] = reg;
  if (reg == 0xd101 || reg == 0xd109) assert(n == 0);
  else {
    assert(n == 1 && data != NULL);
    assert(reg == 0xd005 || reg == 0xd000);
    assert(*(const uint8_t *)data == (reg == 0xd005 ? 0 : 0xab));
  }
  return reg == fail_write ? BUS_ERROR : ESP_OK;
}
'''
    harness += helpers + r'''
static void idle_poll(pet_p4_pointer_decoder_t *out_pointer, pet_p4_touch_decoder_t *out_decoder) {
  pet_p4_pointer_decoder_t pointer = *out_pointer;
  pet_p4_touch_decoder_t decoder = *out_decoder;
  cst328_frame_t frame = {0};
  esp_err_t err = ESP_OK;
  unsigned consecutive_errors = 99;
  int ui_x = decoder.current_x, ui_y = decoder.current_y, last_wake = 0;
  queued = (touch_sample_t){0};
  for (unsigned i = 0; i < 1; ++i) {
'''
    harness += idle_branch + r'''
  }
  assert(consecutive_errors == 0);
  *out_pointer = pointer;
  *out_decoder = decoder;
}
static void reset(void) {
  g_cst328 = false; g_chip_id = 0;
  fail_read = fail_write = -1; write_count = 0;
  memset(report, 0, sizeof(report)); report[6] = 0xab;
  memcpy(check, (uint8_t[]){0, 0, 0xca, 0xca}, 4);
  memcpy(resolution, (uint8_t[]){0xe0, 1, 0x80, 2}, 4);
  memcpy(identity, (uint8_t[]){0x12, 0x34, 0x28, 0x03}, 4);
}
static void assert_ack(void) {
  assert(write_count == 2 && written[0] == 0xd005 && written[1] == 0xd000);
}
int main(void) {
  cst328_frame_t frame;
  reset(); assert(identify_cst328_touch() == ESP_OK);
  assert(g_cst328 && g_chip_id == 0x0328);
  assert(write_count == 4 && written[0] == 0xd101 && written[1] == 0xd109);
  assert(written[2] == 0xd005 && written[3] == 0xd000);
  const int reads[] = {0xd1fc, 0xd1f8, 0xd204};
  for (unsigned i = 0; i < 3; ++i) {
    reset(); fail_read = reads[i];
    assert(identify_cst328_touch() == BUS_ERROR && !g_cst328);
    assert(write_count == 2 && written[1] == 0xd109);
  }
  const int writes[] = {0xd101, 0xd109, 0xd005, 0xd000};
  for (unsigned i = 0; i < 4; ++i) {
    reset(); fail_write = writes[i];
    assert(identify_cst328_touch() == BUS_ERROR && !g_cst328);
    assert(written[1] == 0xd109);
  }
  reset(); check[3] = 0;
  assert(identify_cst328_touch() == ESP_ERR_INVALID_RESPONSE && !g_cst328);
  assert(written[1] == 0xd109);
  reset(); resolution[0] = 0;
  assert(identify_cst328_touch() == ESP_ERR_NOT_SUPPORTED && !g_cst328);
  assert(written[1] == 0xd109);

  reset(); memset(&frame, 0xff, sizeof(frame));
  assert(read_cst328_touch(&frame) == ESP_OK && frame.has_report && frame.count == 0); assert_ack();
  reset(); report[0] = 0xab;
  assert(read_cst328_touch(&frame) == ESP_OK && !frame.has_report && write_count == 0);
  report[5] = 1;
  assert(read_cst328_touch(&frame) == ESP_OK && !frame.has_report && write_count == 0);
  reset(); report[0] = 0x36; report[1] = 29; report[2] = 39;
  report[3] = 0xff; report[5] = 1;
  assert(read_cst328_touch(&frame) == ESP_OK);
  assert(frame.has_report && frame.count == 1 && frame.track_id == 3 && frame.x == 479 && frame.y == 639);
  assert_ack();
  write_count = 0; report[0] = 0x30;
  assert(read_cst328_touch(&frame) == ESP_OK && frame.count == 0); assert_ack();
  write_count = 0; report[0] = 0x34;
  assert(read_cst328_touch(&frame) == ESP_ERR_INVALID_RESPONSE); assert_ack();
  write_count = 0; report[0] = 0x36; report[1] = 30;
  assert(read_cst328_touch(&frame) == ESP_ERR_INVALID_RESPONSE); assert_ack();
  write_count = 0; report[1] = 29; report[2] = 40;
  assert(read_cst328_touch(&frame) == ESP_ERR_INVALID_RESPONSE); assert_ack();
  reset(); report[5] = 2;
  assert(read_cst328_touch(&frame) == ESP_OK && frame.count == 2); assert_ack();
  reset(); report[5] = 5;
  assert(read_cst328_touch(&frame) == ESP_OK && frame.count == 5); assert_ack();
  reset(); report[5] = 6;
  assert(read_cst328_touch(&frame) == ESP_ERR_INVALID_RESPONSE); assert_ack();
  reset(); report[6] = 0;
  assert(read_cst328_touch(&frame) == ESP_ERR_INVALID_RESPONSE && write_count == 0);
  reset(); report[5] = 0x80;
  assert(read_cst328_touch(&frame) == ESP_ERR_INVALID_RESPONSE); assert_ack();
  reset(); fail_read = 0xd000;
  assert(read_cst328_touch(&frame) == BUS_ERROR && write_count == 0);
  reset(); fail_write = 0xd005;
  assert(read_cst328_touch(&frame) == BUS_ERROR); assert_ack();
  reset(); fail_write = 0xd000;
  assert(read_cst328_touch(&frame) == BUS_ERROR); assert_ack();

  pet_p4_pointer_decoder_t pointer = {0};
  pet_p4_touch_decoder_t decoder;
  pet_p4_pointer_event_t event;
  pet_p4_touch_event_t gesture;
  pet_p4_touch_decoder_init(&decoder, 48, 700);
  assert(pet_p4_pointer_update(&pointer, true, true, 100, 200, 10, &event));
  assert(!pet_p4_touch_decoder_update(&decoder, true, 100, 200, 10, &gesture));
  clock_ms = 25; idle_poll(&pointer, &decoder);
  assert(pointer.down && decoder.down && pointer.last_ms == 10 && !queued.has_pointer && !queued.has_gesture);
  clock_ms = 160; idle_poll(&pointer, &decoder);
  assert(pointer.down && !queued.has_pointer);
  clock_ms = 161; idle_poll(&pointer, &decoder);
  assert(!pointer.down && !decoder.down && queued.has_pointer && queued.pointer.kind == PET_P4_POINTER_CANCEL);
  assert(!queued.has_gesture);

  /* Empty polls interleaved with fresh stationary reports preserve long press. */
  pet_p4_touch_decoder_init(&decoder, 48, 700);
  pointer = (pet_p4_pointer_decoder_t){0};
  for (unsigned time = 1000; time <= 1720; time += 30) {
    assert(pet_p4_pointer_update(&pointer, true, true, 100, 200, time, &event));
    bool long_press = pet_p4_touch_decoder_update(&decoder, true, 100, 200, time, &gesture);
    assert(long_press == (time == 1720));
    if (long_press) assert(gesture.gesture == PET_P4_TOUCH_LONG_PRESS);
    clock_ms = time + 15; idle_poll(&pointer, &decoder);
    assert(pointer.down && !queued.has_pointer && !queued.has_gesture);
  }
}
'''
    path = tmp_path / "cst328.c"
    path.write_text(harness, encoding="utf-8")
    binary = tmp_path / ("cst328.exe" if os.name == "nt" else "cst328")
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    assert compiler, "A host C compiler is required"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
    if os.name != "nt":
        flags.append("-fsanitize=address,undefined")
    subprocess.run([compiler, *flags, "-I", str(ROOT / "main"),
                    str(ROOT / "main/pet_p4_touch_core.c"), str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
