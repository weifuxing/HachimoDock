#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
enum {
PET_BUBBLE_TITLE = 1, PET_BUBBLE_BODY = 2, PET_BUBBLE_COMPACT = 3, PET_BUBBLE_BODY_CLIPPED = 4};
typedef struct {
  char            title[192], body[512];
  uint64_t        since_ms, owner_key, target_key;
  bool            pixels, title_bitmap, body_bitmap, glyph_cells;
  bool            body_mask, body_exact, body_pending;
  bool            title_mask, title_pending;
  uint8_t         body_page, body_pages;
  unsigned        style;
  uint32_t        epoch;
  int             title_pin, body_pin;
} pet_bubble_text_view_t;
typedef struct {
  char            id[17], boot[17], client[33], owner[17], version[17];
  char            text[512];
  unsigned        style, page;
  bool            glyph_cells, body_mask;
  const void     *route;
} pet_bubble_need_t;
/* Reserved transport identity, never a serial send context. */
#define PET_BUBBLE_LAN_ROUTE ((const void *)(uintptr_t)UINTPTR_MAX)
bool            pet_bubble_pixels_next_need(uint64_t now_ms, pet_bubble_need_t * out);
bool            pet_bubble_pixels_next_lan_need(uint64_t now_ms, pet_bubble_need_t * out);
void            pet_bubble_pixels_retire_route(const void *route);
bool            pet_bubble_pixels_init(void);
const char     *pet_bubble_pixels_boot(void);
void            pet_bubble_pixels_maintain(uint64_t now_ms, bool pause, bool conversation);
void            pet_bubble_pixels_desire(const cJSON * payload, const void *route, const char *scope, uint64_t now_ms);
bool            pet_bubble_pixels_handle_owned(const cJSON * payload, const void *route, uint64_t now_ms, cJSON * reply);
uint32_t        pet_bubble_pixels_revision(void);
enum {PET_BUBBLE_GLYPH_MISSING = 1, PET_BUBBLE_GLYPH_RESIDENT = 2};
/* Coverage runs only when a bounded text key changes. It must not acquire
 * locks or access the receiver. A null policy selects all nonempty assets. */
void            pet_bubble_pixels_select(const char *owner, unsigned style, const char *title, const char *body, uint64_t now_ms, pet_bubble_text_view_t * out,
    unsigned (*coverage)(const char *text, unsigned style));
void            pet_bubble_pixels_pin_page(pet_bubble_text_view_t * view, unsigned page);
void            pet_bubble_pixels_release(pet_bubble_text_view_t * view);
unsigned        pet_bubble_pixels_pages(unsigned style, const char *text);
bool            pet_bubble_pixels_draw_pinned(const pet_bubble_text_view_t * view, unsigned style, unsigned page, uint16_t * framebuffer, int x, int y, int clip_width, uint16_t color);
/* 16x18, MSB-first alpha cells from the pinned asset for this exact text. */
#define PET_BUBBLE_GLYPH_BYTES 36U
bool            pet_bubble_pixels_copy_glyph(const pet_bubble_text_view_t *view, unsigned style,
    uint32_t codepoint, uint8_t out[PET_BUBBLE_GLYPH_BYTES]);

/* Shared RLE4 coverage compositor. Callers validate the stream dimensions. */
void pet_text_alpha_blit(const uint8_t *data, size_t size, unsigned width,
    uint16_t *framebuffer, int x, int y, int clip_width, uint16_t color);
