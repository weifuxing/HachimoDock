/* Included by the device renderer after the font helpers are defined. */
#include "pet_p4_text_alpha.h"

static void draw_wasm_text(const hachi_text_t *text) {
  const char *cursor = text->utf8;
  float pen = text->x;
  while (*cursor && pen < PET_P4_UI_WIDTH) {
    const uint32_t cp = utf8_next(&cursor), next = utf8_peek(cursor);
    const int advance = glyph_width(cp, next);
    if (!advance) continue;
    lv_font_glyph_dsc_t glyph = {0};
    const int hint = text->px >= 24 ? 2 : 1;
    int source = hint;
    if (!scaled_glyph_dsc(cp, next, &glyph, &source)) {
      source = 1;
      if (!scaled_glyph_dsc('?', 0, &glyph, &source)) continue;
    }
    const float scale = text->px / ((hint == 2 && source == 1) ? 32.0f : 16.0f);
    const float left = pen + glyph.ofs_x * scale;
    const float top = text->y + (scaled_glyph_baseline(&glyph, 1) - glyph.box_h - glyph.ofs_y) * scale;
    pen += advance * (text->px / 16.0f);
    if (!glyph.box_w || !glyph.box_h) continue;
    glyph.req_raw_bitmap = 1;
    const uint8_t *mask = glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
    if (mask) {
      const int x0 = fmaxf(0, floorf(left) - 1), y0 = fmaxf(0, floorf(top) - 1);
      const int x1 = fminf(PET_P4_UI_WIDTH, ceilf(left + glyph.box_w * scale) + 1);
      const int y1 = fminf(PET_P4_UI_HEIGHT, ceilf(top + glyph.box_h * scale) + 1);
      const float inverse = 1.0f / scale;
      for (int y = y0; y < y1; ++y) {
        const float sy = (y + 0.5f - top) * inverse - 0.5f;
        const int iy = (int)floorf(sy);
        const float fy = sy - iy;
        for (int x = x0; x < x1; ++x) {
          const float sx = (x + 0.5f - left) * inverse - 0.5f;
          const int ix = (int)floorf(sx);
          const float fx = sx - ix;
          const float a = app_glyph_sample(mask, &glyph, ix, iy) * (1 - fx)
                        + app_glyph_sample(mask, &glyph, ix + 1, iy) * fx;
          const float b = app_glyph_sample(mask, &glyph, ix, iy + 1) * (1 - fx)
                        + app_glyph_sample(mask, &glyph, ix + 1, iy + 1) * fx;
          const unsigned coverage = (unsigned)((a * (1 - fy) + b * fy) / 17.0f + 0.5f);
          if (coverage) {
            uint16_t *pixel = g_framebuffer + y * PET_P4_UI_WIDTH + x;
            *pixel = pet_text_alpha_blend565(*pixel, text->color, coverage);
          }
        }
      }
    }
    lv_font_glyph_release_draw_data(&glyph);
  }
}
