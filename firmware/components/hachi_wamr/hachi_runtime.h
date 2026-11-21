#ifndef HACHI_RUNTIME_H
#define HACHI_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wasm_export.h"

#define HACHI_AUDIO_PROFILE "hachi-audio-v2"
#if !defined(ESP_PLATFORM) || (defined(PET_P4_WASM_AUDIO) && PET_P4_WASM_AUDIO)
#define HACHI_AUDIO_ENABLED 1
#else
#define HACHI_AUDIO_ENABLED 0
#endif
enum { HACHI_SAMPLE_NONE = -2, HACHI_SAMPLE_STOP = -1, HACHI_SAMPLE_MAX = 64,
       HACHI_MUSIC_FIRST = 64, HACHI_MUSIC_STOP = 128 };

enum { HACHI_W = 320, HACHI_H = 240, HACHI_NATIVE_W = 640, HACHI_NATIVE_H = 480 };
enum { HACHI_MAX_MODULE = 192 * 1024, HACHI_MAX_MEMORY_PAGES = 4, HACHI_MAX_SPRITE = 64 * 1024 };
enum { HACHI_POOL_BYTES = 768 * 1024 };
enum { HACHI_TEXT_LIMIT = 40, HACHI_TEXT_BYTES = 127, HACHI_TEXT_WORK = 240000 };

typedef struct {
    uint16_t x, y, px, color;
    char utf8[HACHI_TEXT_BYTES + 1];
} hachi_text_t;

typedef struct {
    unsigned count;
    hachi_text_t items[HACHI_TEXT_LIMIT];
} hachi_text_layer_t;

typedef struct {
    int32_t values[8];
    uint8_t present;
} hachi_saves_t;

typedef struct {
    int hz, ms, volume;
} hachi_tone_t;

typedef struct {
    uint64_t time_ms;
    int32_t value;
    uint32_t age_ms;
} hachi_snapshot_t;

enum { HACHI_HOST_LOCAL, HACHI_HOST_MONOTONIC, HACHI_HOST_UV };
enum { HACHI_HOST_UNAVAILABLE, HACHI_HOST_READY, HACHI_HOST_STALE };
#define HACHI_HOST_PROFILE "hachi-host-v1"
typedef int (*hachi_host_read_fn)(int kind, hachi_snapshot_t *out);

typedef struct {
    hachi_host_read_fn host_read;
    uint32_t (*host_random)(void);
    uint16_t *screen;
    int width, height;
    hachi_text_layer_t text;
    unsigned text_work;
    bool drawing_frame;
    hachi_saves_t *saves;
    hachi_tone_t tones[8];
    unsigned tone_count;
    unsigned writes;
    unsigned operations;
    unsigned area;
    bool media_allowed;
    int media_command;
    int sample_index, music_index;
    bool sample_event;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t env;
    wasm_function_inst_t init, event, frame;
} hachi_runtime_t;

bool hachi_module_profile(const uint8_t *bytes, size_t size, bool *native_canvas);
bool hachi_module_host_profile(const uint8_t *bytes, size_t size, bool *native_canvas,
                               bool *host_services, bool *audio);
bool hachi_valid_profile(const uint8_t *bytes, size_t size);
const char *hachi_sprite_error(const uint8_t *bytes, size_t size, int width, int height);
bool hachi_runtime_init(void *pool, uint32_t size);
bool hachi_open(hachi_runtime_t *runtime, uint8_t *wasm, uint32_t size,
                uint16_t *screen, size_t screen_pixels, hachi_saves_t *saves, char *error, size_t error_size);
bool hachi_open_host(hachi_runtime_t *runtime, uint8_t *wasm, uint32_t size,
                     uint16_t *screen, size_t screen_pixels, hachi_saves_t *saves,
                     hachi_host_read_fn read, uint32_t (*random)(void), char *error, size_t error_size);
bool hachi_event_run(hachi_runtime_t *runtime, int type, int a, int b);
bool hachi_frame_run(hachi_runtime_t *runtime, int elapsed_ms);
void hachi_close(hachi_runtime_t *runtime);
uint32_t hachi_screen_hash(const uint16_t *screen);

#endif
