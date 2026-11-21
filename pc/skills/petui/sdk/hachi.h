#ifndef HACHI_H
#define HACHI_H

#include <stdint.h>

#if defined(HACHI_NATIVE_CANVAS)
#define HACHI_WIDTH 640
#define HACHI_HEIGHT 480
#else
#define HACHI_WIDTH 320
#define HACHI_HEIGHT 240
#endif

#define HACHI_IMPORT(name) __attribute__((import_module("hachi"), import_name(#name)))
#define HACHI_EXPORT(name) __attribute__((export_name(#name)))

enum {
    HACHI_TOUCH_DOWN = 1,
    HACHI_TOUCH_MOVE = 2,
    HACHI_TOUCH_UP = 3,
    HACHI_KEY_DOWN = 4,
    HACHI_KEY_UP = 5,
    HACHI_TOUCH_CANCEL = 6,
};

enum {
    HACHI_LEFT = 1,
    HACHI_RIGHT,
    HACHI_UP,
    HACHI_DOWN,
    HACHI_A,
    HACHI_B,
    HACHI_SELECT,
};

/* Call once from hachi_init to select native pixel and touch coordinates.
 * Submit single-line UTF-8 in hachi_frame. The host paints it above all graphics. */
HACHI_IMPORT(canvas640) void hachi_canvas640(void);
HACHI_IMPORT(text) void hachi_text(int x, int y, int px, int rgb,
                                   const char *utf8, unsigned bytes);

HACHI_IMPORT(rect) void hachi_rect(int x, int y, int width, int height, int rgb);
HACHI_IMPORT(blit) void hachi_blit(int x, int y, int width, int height, const uint16_t *pixels);
HACHI_IMPORT(sprite) void hachi_sprite(int x, int y, int width, int height,
                                     const uint8_t *asset, unsigned bytes);
HACHI_IMPORT(tone) void hachi_tone(int hz, int milliseconds, int volume);
/* In hachi_event only. Select a cue in assets/speech.hab, or use -1 to stop. */
HACHI_IMPORT(sample) void hachi_sample(int cue);
static inline void hachi_music(int cue) {
    if (cue == -1) hachi_sample(128);
    else if ((unsigned)cue < 64) hachi_sample(64 + cue);
}
HACHI_IMPORT(load) int hachi_load(int slot, int fallback);
HACHI_IMPORT(save) void hachi_save(int slot, int value);
enum { HACHI_MEDIA_PREVIOUS = 1, HACHI_MEDIA_TOGGLE, HACHI_MEDIA_NEXT,
       HACHI_MEDIA_QUIETER, HACHI_MEDIA_LOUDER, HACHI_MEDIA_QUEUE, HACHI_MEDIA_SELECT };
/* Read a bounded host snapshot during hachi_frame. All times are milliseconds.
 * CLOCK_LOCAL uses UTC epoch plus signed UTC offset minutes in value.
 * CLOCK_MONOTONIC uses time since device boot, with value and age_ms zero.
 * UV uses sample UTC epoch, UV index × 100 in value, and sample age.
 * The output is zeroed when unavailable. Status: 0 unavailable, 1 ready, 2 stale. */
typedef struct {
    uint64_t time_ms;
    int32_t value;
    uint32_t age_ms;
} hachi_snapshot_t;

enum { HACHI_CLOCK_LOCAL = 0, HACHI_CLOCK_MONOTONIC = 1 };
enum { HACHI_UNAVAILABLE = 0, HACHI_READY = 1, HACHI_STALE = 2 };
HACHI_IMPORT(clock_read) int hachi_clock_read(int clock, hachi_snapshot_t *out);
HACHI_IMPORT(uv_read) int hachi_uv_read(hachi_snapshot_t *out);
/* A fresh unsigned value from the host, for games and visual effects. */
HACHI_IMPORT(random_u32) uint32_t hachi_random_u32(void);

HACHI_IMPORT(media_cmd) void hachi_media_cmd(int command);

HACHI_EXPORT(hachi_init) void hachi_init(void);
HACHI_EXPORT(hachi_event) void hachi_event(int type, int a, int b);
HACHI_EXPORT(hachi_frame) void hachi_frame(int elapsed_ms);

#endif
