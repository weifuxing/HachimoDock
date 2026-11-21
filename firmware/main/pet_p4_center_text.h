#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PET_CENTER_TITLE_MASK_MAX = 6144, PET_CENTER_TITLE_MASK_HEIGHT = 33,
       PET_CENTER_TITLE_MASK_WIDTH = 252, PET_CENTER_MASK_HEADER = 3 };

static inline unsigned pet_center_mask_width(const uint8_t *data) {
  return (unsigned)data[0] | ((unsigned)data[1] << 8);
}

static inline bool pet_center_title_mask_valid(const uint8_t *data, size_t length) {
  if (length <= PET_CENTER_MASK_HEADER || length > PET_CENTER_TITLE_MASK_MAX ||
      data[2] != PET_CENTER_TITLE_MASK_HEIGHT) return false;
  const unsigned width = pet_center_mask_width(data);
  if (width < 4 || width > PET_CENTER_TITLE_MASK_WIDTH) return false;
  unsigned pixels = 0;
  for (size_t i = PET_CENTER_MASK_HEADER; i < length; ++i) pixels += (data[i] & 15u) + 1u;
  return pixels == width * PET_CENTER_TITLE_MASK_HEIGHT;
}
