/*
 * [Input] P4 LCD panel operations, boot lifecycle, and RGB565 frame buffers.
 * [Output] DCS-gated first-frame reveal and bounded draws on the fixed-backlight adapter.
 * [Pos] Public LCD contract for the ESP32-P4 runtime.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_p4_lcd_profile.h"

esp_err_t pet_p4_lcd_prepare_boot(void);
esp_err_t pet_p4_lcd_init(void);
esp_err_t pet_p4_lcd_reveal(void);
esp_err_t pet_p4_lcd_backlight_status(void);
esp_err_t pet_p4_lcd_keep_awake(void);
esp_err_t pet_p4_lcd_draw_rgb565(int x, int y, int width, int height, const uint16_t *pixels);
#if PET_P4_RENDER_LCD_SPLIT_PROBE
/* Native renderer observation only; same single submit/fence/return behavior. */
esp_err_t pet_p4_lcd_draw_rgb565_observed(int x,int y,int width,int height,const uint16_t *pixels,pet_lcd_observation_t *out);
#endif
int pet_p4_lcd_scan_i2c(char *buffer, size_t buffer_size);

#if PET_P4_SYSTEM_WIFI
enum pet_lcd_phase {
  PET_LCD_BETWEEN_FRAMES, PET_LCD_COMPOSE, PET_LCD_ROTATE, PET_LCD_COMPARE, PET_LCD_SUBMIT
};
typedef struct {
  uint32_t count, last_tick, last_phase;
  bool video_worker;
} pet_lcd_underrun_t;

void pet_p4_lcd_render_phase(enum pet_lcd_phase phase);
void pet_p4_lcd_video_worker(bool active);
uint32_t pet_p4_lcd_underrun_count(void);
pet_lcd_underrun_t pet_p4_lcd_underrun(void);
#endif
