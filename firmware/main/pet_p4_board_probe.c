#include "pet_p4_board_probe.h"

#include <stdint.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

/* Waveshare ESP32-P4-WIFI6 schematic, U14. Leave DAT0 (Pico SW1), the
 * other DAT pins and C6 reset alone. No SDIO register writes or RF setup. */
#define C6_CLK GPIO_NUM_18
#define C6_CMD GPIO_NUM_19
#define C6_VOLTAGE_MASK 0x00300000u
#define C6_PROBE_US 45000

static atomic_uint probe_phase;
unsigned pet_p4_board_probe_phase(void) { return atomic_load(&probe_phase); }

static unsigned clock_cmd(void) {
  esp_rom_delay_us(3);
  gpio_set_level(C6_CLK, 1);
  esp_rom_delay_us(3);
  unsigned bit = gpio_get_level(C6_CMD);
  gpio_set_level(C6_CLK, 0);
  return bit;
}

static uint8_t crc7_stop(const uint8_t frame[6]) {
  uint8_t crc = 0;
  for (unsigned byte = 0; byte < 5; ++byte) {
    for (unsigned bit = 0x80; bit; bit >>= 1) {
      bool feedback = ((crc & 0x40) != 0) != ((frame[byte] & bit) != 0);
      crc = ((crc << 1) & 0x7f) ^ (feedback ? 0x09 : 0);
    }
  }
  return (crc << 1) | 1;
}

static bool command(unsigned index, uint32_t arg, uint32_t *payload, int64_t deadline) {
  if (esp_timer_get_time() >= deadline) return false;
  uint8_t frame[6] = {0x40 | index, arg >> 24, arg >> 16, arg >> 8, arg, 0};
  frame[5] = crc7_stop(frame);
  for (unsigned byte = 0; byte < sizeof(frame); ++byte) {
    for (unsigned bit = 0x80; bit; bit >>= 1) {
      gpio_set_level(C6_CMD, (frame[byte] & bit) != 0);
      clock_cmd();
    }
  }
  gpio_set_level(C6_CMD, 1);
  if (!payload) return true;
  unsigned wait = 0;
  while (clock_cmd()) {
    if (++wait == 64) return false;
  }
  frame[0] = 0; // The first response bit (start) was sampled above.
  for (unsigned bit = 1; bit < 48; ++bit) {
    if (bit % 8 == 0) frame[bit / 8] = 0;
    frame[bit / 8] = (frame[bit / 8] << 1) | clock_cmd();
  }
  for (unsigned i = 0; i < 8; ++i) clock_cmd();
  if (index == 5) {
    if (frame[0] != 0x3f || frame[5] != 0xff) return false; // R4 has no CRC.
  } else if (frame[0] != index || frame[5] != crc7_stop(frame)) {
    return false;
  }
  *payload = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) |
             ((uint32_t)frame[3] << 8) | frame[4];
  return true;
}

static bool read_byte(uint32_t address, uint8_t *value, int64_t deadline) {
  uint32_t response;
  // CMD52, read, function 0. The R5 status mask excludes the I/O state bits.
  if (address >= 0x20000 || !command(52, address << 9, &response, deadline) ||
      (response & 0xcb00)) return false;
  *value = response;
  return true;
}

static bool hosted_cis(uint32_t pointer_register, int64_t deadline) {
  uint32_t address = 0;
  for (unsigned i = 0; i < 3; ++i) {
    uint8_t value;
    if (!read_byte(pointer_register + i, &value, deadline)) return false;
    address |= (uint32_t)value << (i * 8);
  }
  if (address < 0x1000 || address >= 0x20000) return false;
  for (unsigned tuple = 0; tuple < 16; ++tuple) {
    uint8_t code, length;
    if (!read_byte(address++, &code, deadline) || code == 0xff) return false;
    if (code == 0) continue;
    if (!read_byte(address++, &length, deadline) || length == 0xff) return false;
    if (code == 0x20 && length == 4) { // CISTPL_MANFID
      uint8_t id[4];
      for (unsigned i = 0; i < sizeof(id); ++i) {
        if (!read_byte(address + i, &id[i], deadline)) return false;
      }
      return id[0] == 0x92 && id[1] == 0 && id[2] == id[3] &&
             (id[2] == 0x66 || id[2] == 0x77);
    }
    address += length;
  }
  return false;
}

static bool probe(int64_t deadline) {
  for (unsigned i = 0; i < 80; ++i) clock_cmd();
  // CMD0 initializes SD command state. No C6 reset pin or download mode.
  if (!command(0, 0, NULL, deadline)) return false;
  for (unsigned i = 0; i < 8; ++i) clock_cmd();
  atomic_store(&probe_phase,2);
  uint32_t ocr;
  if (!command(5, 0, &ocr, deadline) || !(ocr & 0x70000000) ||
      (ocr & 0x08000000) || !(ocr & C6_VOLTAGE_MASK)) return false;
  atomic_store(&probe_phase,3);
  uint32_t voltage = ocr & C6_VOLTAGE_MASK;
  while (esp_timer_get_time() < deadline) {
    if (!command(5, voltage, &ocr, deadline)) return false;
    if (ocr & 0x80000000) {
      atomic_store(&probe_phase,4);
      uint32_t rca, selected;
      if (!command(3, 0, &rca, deadline) || !(rca & 0xffff0000) ||
          (rca & 0xe000) || !command(7, rca & 0xffff0000, &selected, deadline)) {
        return false;
      }
      atomic_store(&probe_phase,5);
      bool found = hosted_cis(0x09, deadline) || hosted_cis(0x109, deadline);
      (void)command(7, 0, NULL, deadline);
      if (found) atomic_store(&probe_phase,6);
      return found;
    }
  }
  return false;
}

bool pet_p4_probe_wifi6_board(void) {
  atomic_store(&probe_phase,1);
  const gpio_config_t clock = {
    .pin_bit_mask = 1ULL << C6_CLK,
#if PET_P4_SYSTEM_WIFI
    .mode = GPIO_MODE_OUTPUT,
#else
    .mode = GPIO_MODE_INPUT_OUTPUT_OD,
    .pull_up_en = GPIO_PULLUP_ENABLE,
#endif
  };
  const gpio_config_t command_pin = {
    .pin_bit_mask = 1ULL << C6_CMD,
    .mode = GPIO_MODE_INPUT_OUTPUT_OD,
    .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_set_level(C6_CLK, 0);
  gpio_set_level(C6_CMD, 1);
  esp_err_t setup = gpio_config(&command_pin);
  if (setup == ESP_OK) setup = gpio_config(&clock);
  const int64_t started = esp_timer_get_time();
  bool found = setup == ESP_OK && probe(started + C6_PROBE_US);
  if (!found) {
    ESP_LOGW("p4-board-probe", "SDIO probe phase %u, GPIO %s, elapsed %lld us",
             pet_p4_board_probe_phase(), esp_err_to_name(setup),
             (long long)(esp_timer_get_time() - started));
  }
  gpio_reset_pin(C6_CLK);
  gpio_reset_pin(C6_CMD);
  gpio_set_pull_mode(C6_CLK, GPIO_FLOATING);
  gpio_set_pull_mode(C6_CMD, GPIO_FLOATING);
  return found;
}
