"""Execute the production touch task with deterministic GPIO/I2C/RTOS fakes."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_cst328_interrupt_task(tmp_path):
    source = (ROOT / "main/pet_p4_touch.c").read_text(encoding="utf-8")
    globals_source = source.split("#define PET_P4_TOUCH_SAMPLE_MS", 1)[1]
    globals_source = "#define PET_P4_TOUCH_SAMPLE_MS" + globals_source.split("void pet_p4_touch_health", 1)[0]
    functions = source.split("static void queue_sample", 1)[1]
    functions = "static void queue_sample" + functions.split("static const char *touch_event_name", 1)[0]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
#include "pet_p4_touch_core.h"
typedef int esp_err_t, BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t, *QueueHandle_t, *esp_lcd_touch_handle_t;
typedef void *esp_lcd_panel_io_handle_t, *i2c_master_bus_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_TIMEOUT 5
#define ESP_ERR_NO_MEM 6
#define BUS_ERROR 7
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY UINT32_MAX
#define IRAM_ATTR
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define GPIO_NUM_NC (-1)
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_NEGEDGE 2
#define BSP_LCD_H_RES 480
#define BSP_LCD_V_RES 640
#define BSP_LCD_POWER_ON_DELAY_MS 1000
#define BSP_I2C_SDA 7
#define BSP_I2C_SCL 8
#define CONFIG_BSP_I2C_CLK_SPEED_HZ 400000
#define ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS 0x5d
#define ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP 0x14
#define ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() ((esp_lcd_panel_io_i2c_config_t){.lcd_cmd_bits=16})
static int fake_interrupt_gpio = 22;
#define BSP_LCD_TOUCH_INT fake_interrupt_gpio
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct {
  int dev_addr, scl_speed_hz, control_phase_bytes, lcd_cmd_bits;
  struct { int disable_control_phase; } flags;
} esp_lcd_panel_io_i2c_config_t;
typedef struct {
  int x_max, y_max, rst_gpio_num, int_gpio_num;
  struct { int reset, interrupt; } levels;
  struct { int swap_xy, mirror_x, mirror_y; } flags;
} esp_lcd_touch_config_t;
typedef struct { int track_id; uint16_t x, y; } esp_lcd_touch_point_data_t;
static void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken);
static void yield_from_isr(void);
#define portYIELD_FROM_ISR() yield_from_isr()
static int gpio_config(const gpio_config_t *config);
static int gpio_install_isr_service(int flags);
static int gpio_isr_handler_add(int gpio, void (*handler)(void *), void *arg);
static void gpio_uninstall_isr_service(void);
static int gpio_reset_pin(int gpio);
static void xTaskNotifyGive(TaskHandle_t task);
static unsigned ulTaskNotifyTake(int clear, TickType_t ticks);
static void vTaskDelay(TickType_t ticks);
static void vTaskDelayUntil(TickType_t *wake, TickType_t ticks);
static TickType_t xTaskGetTickCount(void);
static int64_t esp_timer_get_time(void);
static int xQueueSend(QueueHandle_t queue, const void *sample, int ticks);
static void xQueueReset(QueueHandle_t queue);
static QueueHandle_t xQueueCreate(int count, size_t size);
static void vQueueDelete(QueueHandle_t queue);
static int xTaskCreate(void (*task)(void *), const char *name, int stack, void *arg,
                       int priority, TaskHandle_t *handle);
static void vTaskDelete(TaskHandle_t task);
static int esp_lcd_panel_io_rx_param(void *io, int reg, void *data, size_t len);
static int esp_lcd_panel_io_tx_param(void *io, int reg, const void *data, size_t len);
static int esp_lcd_touch_read_data(void *touch) { (void)touch; return ESP_OK; }
static int esp_lcd_touch_get_data(void *touch, esp_lcd_touch_point_data_t *points, uint8_t *count, int max) {
  (void)touch; (void)points; (void)max; *count=0; return ESP_OK;
}
static int bsp_i2c_init(void) { return ESP_OK; }
static void *bsp_i2c_get_handle(void) { return (void *)1; }
static int i2c_master_probe(void *bus, int addr, int timeout) {
  (void)bus; (void)timeout; return addr == 0x1a ? ESP_OK : ESP_ERR_NOT_FOUND;
}
static int esp_lcd_new_panel_io_i2c(void *bus, const esp_lcd_panel_io_i2c_config_t *config, void **io) {
  (void)bus; assert(config->dev_addr == 0x1a && config->lcd_cmd_bits == 16); *io=(void *)2; return ESP_OK;
}
static int esp_lcd_touch_new_i2c_gt911(void *io, const esp_lcd_touch_config_t *config, void **touch) {
  (void)io; (void)config; (void)touch; assert(false); return ESP_OK;
}
static void esp_lcd_touch_del(void *touch) { (void)touch; }
static int io_deletes;
static void esp_lcd_panel_io_del(void *io) { assert(io); io_deletes++; }
'''
    harness += globals_source + functions + r'''
typedef struct { uint64_t time; uint8_t bytes[27]; } scheduled_report_t;
static scheduled_report_t scheduled[8];
static int scheduled_count, scheduled_next;
static uint8_t report[27];
static uint64_t clock_ms, poll_stop_ms;
static unsigned notifications, waits, isr_yields, isr_calls;
static unsigned reads, writes, delay_calls, poll_delays;
static int read_failures, write_failures;
static bool notify_in_read, notify_on_last_failure, isr_wakes_task;
static int gpio_error, service_error, handler_error, queue_error, task_error;
static int gpio_resets, service_uninstalls, task_deletes, queue_deletes;
static void (*registered_handler)(void *);
static void *registered_arg;
static touch_sample_t samples[64];
static int sample_count;
static jmp_buf stopped;
static void reset(void) {
  g_ready=false; g_cst328=true; g_ft6336=g_cst826=false; g_touch_interrupt_mode=true;
  g_touch=NULL; g_touch_io=(void *)2; g_touch_task=(void *)3; g_touch_queue=(void *)4;
  atomic_store(&g_consecutive_errors,0); atomic_store(&g_poll_errors,0);
  scheduled_count=scheduled_next=0; clock_ms=poll_stop_ms=0;
  notifications=waits=isr_yields=isr_calls=0; reads=writes=delay_calls=poll_delays=0;
  read_failures=write_failures=0; notify_in_read=notify_on_last_failure=false;
  isr_wakes_task=true; gpio_error=service_error=handler_error=queue_error=task_error=0;
  gpio_resets=service_uninstalls=task_deletes=queue_deletes=io_deletes=0;
  registered_handler=NULL; registered_arg=NULL; sample_count=0; fake_interrupt_gpio=22;
  memset(report,0,sizeof(report)); report[0]=report[6]=0xab;
}
static void schedule(uint64_t time, uint8_t status, uint8_t count) {
  assert(scheduled_count < 8);
  scheduled_report_t *event=&scheduled[scheduled_count++];
  memset(event,0,sizeof(*event)); event->time=time;
  event->bytes[0]=status; event->bytes[1]=6; event->bytes[2]=12;
  event->bytes[3]=0x48; event->bytes[5]=count; event->bytes[6]=0xab;
}
static void load_next(void) {
  memcpy(report,scheduled[scheduled_next].bytes,sizeof(report)); scheduled_next++;
}
static unsigned ulTaskNotifyTake(int clear, TickType_t ticks) {
  assert(clear == pdTRUE); waits++;
  if (notifications) { unsigned value=notifications; notifications=0; return value; }
  if (scheduled_next < scheduled_count &&
      (ticks == portMAX_DELAY || scheduled[scheduled_next].time <= clock_ms + ticks)) {
    clock_ms=scheduled[scheduled_next].time; load_next(); return 1;
  }
  if (ticks == portMAX_DELAY) longjmp(stopped,1);
  clock_ms+=ticks; return 0;
}
static void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken) {
  assert(task == g_touch_task); notifications++; isr_calls++; *woken=isr_wakes_task;
}
static void yield_from_isr(void) { isr_yields++; }
static void xTaskNotifyGive(TaskHandle_t task) { assert(task == g_touch_task); notifications++; }
static int gpio_config(const gpio_config_t *config) {
  assert(config->pin_bit_mask == (1ULL << 22)); assert(config->mode == GPIO_MODE_INPUT);
  assert(config->pull_up_en == GPIO_PULLUP_ENABLE && config->intr_type == GPIO_INTR_NEGEDGE);
  assert(g_touch_interrupt_mode && g_touch_task); return gpio_error;
}
static int gpio_install_isr_service(int flags) { assert(flags == 0); return service_error; }
static int gpio_isr_handler_add(int gpio, void (*handler)(void *), void *arg) {
  assert(gpio == 22 && arg == g_touch_task);
  if (handler_error) return handler_error;
  registered_handler=handler; registered_arg=arg; return ESP_OK;
}
static void gpio_uninstall_isr_service(void) { service_uninstalls++; }
static int gpio_reset_pin(int gpio) { assert(gpio == 22); gpio_resets++; return ESP_OK; }
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms*1000; }
static TickType_t xTaskGetTickCount(void) { return (TickType_t)clock_ms; }
static void vTaskDelay(TickType_t ticks) { clock_ms+=ticks; delay_calls++; }
static void vTaskDelayUntil(TickType_t *wake, TickType_t ticks) {
  *wake+=ticks; clock_ms=*wake; poll_delays++;
  if (poll_stop_ms && clock_ms >= poll_stop_ms) longjmp(stopped,1);
  if (scheduled_next < scheduled_count && scheduled[scheduled_next].time <= clock_ms) load_next();
}
static int xQueueSend(QueueHandle_t queue, const void *sample, int ticks) {
  (void)ticks; assert(queue && sample_count < 64); samples[sample_count++]=*(const touch_sample_t *)sample; return pdTRUE;
}
static void xQueueReset(QueueHandle_t queue) { (void)queue; sample_count=0; }
static QueueHandle_t xQueueCreate(int count, size_t size) {
  assert(count == 32 && size == sizeof(touch_sample_t)); return queue_error ? NULL : (void *)4;
}
static void vQueueDelete(QueueHandle_t queue) { assert(queue); queue_deletes++; }
static int xTaskCreate(void (*task)(void *), const char *name, int stack, void *arg, int priority, TaskHandle_t *handle) {
  (void)name; (void)stack; (void)arg; (void)priority;
  assert(task == touch_task); if (task_error) return 0; *handle=(void *)3; return pdPASS;
}
static void vTaskDelete(TaskHandle_t task) { assert(task); task_deletes++; }
static int esp_lcd_panel_io_rx_param(void *io, int reg, void *data, size_t len) {
  assert(io);
  if (reg == 0xd000) {
    assert(len == 27); reads++;
    if (notify_in_read) { notify_in_read=false; touch_interrupt_handler(g_touch_task); }
    if (read_failures) {
      read_failures--;
      if (!read_failures && notify_on_last_failure) touch_interrupt_handler(g_touch_task);
      return BUS_ERROR;
    }
    memcpy(data,report,len); return ESP_OK;
  }
  assert(len == 4);
  if (reg == 0xd1fc) memcpy(data,(uint8_t[]){0,0,0xca,0xca},4);
  else if (reg == 0xd1f8) memcpy(data,(uint8_t[]){0xe0,1,0x80,2},4);
  else if (reg == 0xd204) memcpy(data,(uint8_t[]){0,0,0x28,3},4);
  else assert(false);
  return ESP_OK;
}
static int esp_lcd_panel_io_tx_param(void *io, int reg, const void *data, size_t len) {
  assert(io); writes++;
  if (reg == 0xd101 || reg == 0xd109) { assert(len == 0); return ESP_OK; }
  assert(len == 1);
  if (write_failures) { write_failures--; return BUS_ERROR; }
  if (reg == 0xd000) { assert(*(const uint8_t *)data == 0xab); report[0]=0xab; }
  else { assert(reg == 0xd005 && *(const uint8_t *)data == 0); report[5]=0; }
  return ESP_OK;
}
static void run_task(void) { if (!setjmp(stopped)) touch_task(NULL); }
static void assert_pointer(int index, pet_p4_pointer_kind_t kind) {
  assert(index < sample_count && samples[index].has_pointer && samples[index].pointer.kind == kind);
}
int main(void) {
  reset(); run_task(); assert(reads == 0 && writes == 0 && waits == 1 && sample_count == 0);
  reset(); assert(init_touch_interrupt() == ESP_OK);
  assert(notifications == 1 && registered_handler && registered_arg == g_touch_task);
  registered_handler(registered_arg); assert(notifications == 2 && isr_yields == 1);
  isr_wakes_task=false; registered_handler(registered_arg); assert(notifications == 3 && isr_yields == 1);
  run_task(); assert(reads == 1 && writes == 0); // Coalesced duplicate IRQs consume one snapshot.

  reset(); schedule(0,0,0); notify_in_read=true; run_task();
  assert(reads == 2 && writes == 2 && isr_calls == 1 && notifications == 0);

  reset(); schedule(0,0x06,1); schedule(900,0,0); run_task();
  assert(reads == 2 && writes == 4 && delay_calls == 0 && poll_delays == 0);
  assert(sample_count == 3); assert_pointer(0,PET_P4_POINTER_DOWN);
  assert(samples[1].has_gesture && samples[1].gesture.gesture == PET_P4_TOUCH_LONG_PRESS);
  assert(samples[1].gesture.ts_ms >= 700 && samples[1].gesture.ts_ms < 710);
  assert_pointer(2,PET_P4_POINTER_UP);

  reset(); schedule(0,0x06,1); schedule(200,0xab,0); schedule(500,0,0); run_task();
  assert(reads == 3 && writes == 4 && sample_count == 2);
  assert_pointer(0,PET_P4_POINTER_DOWN); assert_pointer(1,PET_P4_POINTER_UP);
  assert(samples[1].has_gesture && samples[1].gesture.gesture == PET_P4_TOUCH_TAP);

  reset(); g_touch_interrupt_mode=false; schedule(0,0x06,1); load_next(); poll_stop_ms=180; run_task();
  assert(waits == 0 && reads == 12 && poll_delays == 12 && writes == 2);
  assert(sample_count == 2); assert_pointer(1,PET_P4_POINTER_CANCEL);

  reset(); schedule(0,0,0); read_failures=1; run_task();
  assert(reads == 2 && delay_calls == 1 && writes == 2 && atomic_load(&g_consecutive_errors) == 0);
  reset(); g_ready=true; schedule(0,0,0); read_failures=3; run_task();
  assert(reads == 3 && delay_calls == 2 && writes == 2);
  assert(atomic_load(&g_consecutive_errors) == 3 && !pet_p4_touch_ready());
  reset(); g_ready=true; schedule(0,0x06,1); write_failures=8; run_task();
  assert(reads == 3 && delay_calls == 2 && writes == 8 && !pet_p4_touch_ready());
  reset(); g_ready=true; schedule(0,0,0); read_failures=3; notify_on_last_failure=true; run_task();
  assert(reads == 4 && writes == 2 && !pet_p4_touch_ready());

  for (int failure=0; failure<5; failure++) {
    reset(); g_touch_io=g_touch_task=g_touch_queue=NULL; clock_ms=1000;
    if (failure == 0) queue_error=1;
    if (failure == 1) task_error=1;
    if (failure == 2) gpio_error=BUS_ERROR;
    if (failure == 3) service_error=BUS_ERROR;
    if (failure == 4) handler_error=BUS_ERROR;
    assert(pet_p4_touch_init() != ESP_OK && !g_ready && !g_touch_interrupt_mode);
    assert(!g_touch_task && !g_touch_queue && !g_touch_io && io_deletes == 1);
    assert(task_deletes == (failure >= 2) && queue_deletes == (failure >= 1));
    if (failure >= 2) assert(gpio_resets == 1);
    assert(service_uninstalls == (failure == 4));
  }
  reset(); g_touch_io=g_touch_task=g_touch_queue=NULL; clock_ms=1000;
  service_error=ESP_ERR_INVALID_STATE; handler_error=BUS_ERROR;
  assert(pet_p4_touch_init() == BUS_ERROR && service_uninstalls == 0 && gpio_resets == 1);
  reset(); g_touch_io=g_touch_task=g_touch_queue=NULL; clock_ms=1000;
  assert(pet_p4_touch_init() == ESP_OK && g_ready && g_touch_interrupt_mode && notifications == 1);
  reset(); g_touch_io=g_touch_task=g_touch_queue=NULL; clock_ms=1000; fake_interrupt_gpio=GPIO_NUM_NC;
  assert(pet_p4_touch_init() == ESP_OK && g_ready && !g_touch_interrupt_mode && !registered_handler && !notifications);
}
'''
    path = tmp_path / "cst328_interrupt.c"
    path.write_text(harness, encoding="utf-8")
    binary = tmp_path / ("cst328_interrupt.exe" if os.name == "nt" else "cst328_interrupt")
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    assert compiler, "A host C compiler is required"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable"]
    if os.name != "nt":
        flags.append("-fsanitize=address,undefined")
    subprocess.run([compiler, *flags, "-I", str(ROOT / "main"),
                    str(ROOT / "main/pet_p4_touch_core.c"), str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
