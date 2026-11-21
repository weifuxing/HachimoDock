#pragma once
#include <stdint.h>

static inline int pet_p4_bluetooth_hex(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

// hid-<modifier mask>-<keyboard usage>, using lowercase hexadecimal.
static inline uint16_t pet_p4_bluetooth_chord(const char *value) {
  if (!value || __builtin_strlen(value) != 9 ||
      __builtin_memcmp(value, "hid-", 4) || value[6] != '-') return 0;
  int modifier = pet_p4_bluetooth_hex(value[5]);
  int high = pet_p4_bluetooth_hex(value[7]);
  int low = pet_p4_bluetooth_hex(value[8]);
  if (value[4] != '0' || modifier <= 0 || high < 0 || low < 0) return 0;
  unsigned key = (unsigned)(high * 16 + low);
  if (!((key >= 0x04 && key <= 0x52) || key == 0x65)) return 0;
  return (uint16_t)((modifier << 8) | key);
}
