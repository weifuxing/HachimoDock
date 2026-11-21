#pragma once
#include <stdint.h>

static inline uint16_t pet_text_alpha_blend565(uint16_t background, uint16_t color, unsigned alpha){
  if (alpha == 15)
    return color;
  unsigned        inverse = 15 - alpha;
  unsigned        r = (((color >> 11) & 31) * alpha + ((background >> 11) & 31) * inverse + 7) / 15;
  unsigned        g = (((color >> 5) & 63) * alpha + ((background >> 5) & 63) * inverse + 7) / 15;
  unsigned        b = ((color & 31) * alpha + (background & 31) * inverse + 7) / 15;
  return (uint16_t) ((r << 11) | (g << 5) | b);
}
