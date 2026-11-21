#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  PET_MQTT_BODY_LIMIT = 16384 + 2 + 127,
  PET_MQTT_REMAINING_BYTES = 3,
};

/* Bounded MQTT 3.1.1 Remaining Length. Reject noncanonical lengths and
 * bodies that cannot be consumed by this read-only client. */
static inline size_t pet_mqtt_length_encode(uint8_t out[PET_MQTT_REMAINING_BYTES], size_t size) {
  if (size > PET_MQTT_BODY_LIMIT) return 0;
  size_t count = 0;
  do {
    const unsigned digit = size % 128;
    size /= 128;
    out[count++] = digit | (size ? 128 : 0);
  } while (size);
  return count;
}

static inline bool pet_mqtt_length_digit(size_t *length, unsigned index, uint8_t byte) {
  if (index >= PET_MQTT_REMAINING_BYTES) return false;
  if ((byte & 128) && index + 1 == PET_MQTT_REMAINING_BYTES) return false;
  if (!(byte & 128) && index && !(byte & 127)) return false;
  *length |= (size_t)(byte & 127) << (7 * index);
  return *length <= PET_MQTT_BODY_LIMIT;
}

static inline size_t pet_mqtt_u16(const uint8_t *data) {
  return (size_t)data[0] << 8 | data[1];
}
