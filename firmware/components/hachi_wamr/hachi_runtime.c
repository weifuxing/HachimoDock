#include "hachi_runtime.h"

#include <stdio.h>
#include <string.h>

static bool read_u32(const uint8_t **position, const uint8_t *end, uint32_t *value) {
    *value = 0;
    for (unsigned shift = 0; shift <= 28 && *position < end; shift += 7) {
        unsigned byte = *(*position)++;
        if (shift == 28 && byte > 15) return false;
        *value |= (byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}

static const uint8_t *read_name(const uint8_t **p, const uint8_t *end, uint32_t *length) {
    if (!read_u32(p, end, length) || *length > (size_t)(end - *p)) return NULL;
    const uint8_t *name = *p;
    *p += *length;
    return name;
}

bool hachi_module_host_profile(const uint8_t *bytes, size_t size, bool *native_canvas,
bool *host_services, bool *audio) {
    static const uint8_t header[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    if (size < sizeof(header) || size > HACHI_MAX_MODULE ||
        memcmp(bytes, header, sizeof(header)) != 0) return false;
    const uint8_t *p = bytes + sizeof(header);
    const uint8_t *end = bytes + size;
    bool has_memory = false, has_imports = false;
    unsigned imports_seen = 0;
    static const char *const names[] = {
        "canvas640", "text", "rect", "blit", "sprite", "tone", "load", "save", "media_cmd",
        "clock_read", "uv_read", "random_u32",
#if HACHI_AUDIO_ENABLED
        "sample",
#endif
    };
    while (p < end) {
        unsigned id = *p++;
        uint32_t length;
        if (!read_u32(&p, end, &length) || length > (size_t)(end - p) || id == 8)
            return false;
        const uint8_t *section_end = p + length;
        if (id == 2) {
            uint32_t count;
            if (has_imports || !read_u32(&p, section_end, &count) || count > sizeof(names) / sizeof(names[0])) return false;
            has_imports = true;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t length, type;
                const uint8_t *name = read_name(&p, section_end, &length);
                if (!name || length != 5 || memcmp(name, "hachi", 5)) return false;
                name = read_name(&p, section_end, &length);
                if (!name) return false;
                bool allowed = false;
                for (unsigned j = 0; j < sizeof(names) / sizeof(names[0]); ++j) {
                    if (length != strlen(names[j]) || memcmp(name, names[j], length)) continue;
                    if (imports_seen & (1u << j)) return false;
                    imports_seen |= 1u << j;
                    allowed = true;
                    break;
                }
                if (!allowed || p == section_end || *p++ != 0 ||
                    !read_u32(&p, section_end, &type)) return false;
            }
            if (p != section_end) return false;
        }
        if (id == 5) {
            uint32_t count, flags, initial, maximum;
            if (has_memory || !read_u32(&p, section_end, &count) || count != 1 ||
                !read_u32(&p, section_end, &flags) || flags != 1 ||
                !read_u32(&p, section_end, &initial) ||
                (initial != 2 && initial != HACHI_MAX_MEMORY_PAGES) ||
                !read_u32(&p, section_end, &maximum) || maximum != initial ||
                p != section_end) return false;
            has_memory = true;
        }
        p = section_end;
    }
    bool native = (imports_seen & 1u) != 0;
    bool has_audio = (imports_seen & (1u << 12)) != 0;
    if (!has_memory || ((imports_seen & 2u) && !native)) return false;
    if (native_canvas) *native_canvas = native;
    if (host_services) *host_services = ((imports_seen >> 9) & 7u) != 0;
    if (audio) *audio = has_audio;
    return true;
}

bool hachi_module_profile(const uint8_t *bytes, size_t size, bool *native_canvas) {
    return hachi_module_host_profile(bytes, size, native_canvas, NULL, NULL);
}

bool hachi_valid_profile(const uint8_t *bytes, size_t size) {
    return hachi_module_profile(bytes, size, NULL);
}

typedef struct { int left, top, right, bottom; } region_t;

static bool region(hachi_runtime_t *rt, wasm_exec_env_t env, int x, int y,
                   int w, int h, region_t *box) {
    if (++rt->operations > 128) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "drawing operation limit");
        return false;
    }
    if (w < 1 || h < 1 || w > rt->width || h > rt->height ||
        x < -rt->width || x >= rt->width || y < -rt->height || y >= rt->height)
        return false;
    *box = (region_t){x < 0 ? 0 : x, y < 0 ? 0 : y,
                      x + w > rt->width ? rt->width : x + w,
                      y + h > rt->height ? rt->height : y + h};
    if (box->right <= box->left || box->bottom <= box->top) return false;
    rt->area += (unsigned)((box->right - box->left) * (box->bottom - box->top));
    if (rt->area > (unsigned)(rt->width * rt->height) * 4u) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "drawing area limit");
        return false;
    }
    return true;
}

static void rect(wasm_exec_env_t env, int32_t x, int32_t y,
                 int32_t w, int32_t h, int32_t rgb) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    region_t box;
    if (!region(rt, env, x, y, w, h, &box)) return;
    unsigned color = (unsigned)rgb;
    uint16_t pixel = (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) |
                                ((color >> 3) & 0x1f));
    for (int row = box.top; row < box.bottom; ++row) {
        for (int column = box.left; column < box.right; ++column)
            rt->screen[row * rt->width + column] = pixel;
    }
}

static void blit(wasm_exec_env_t env, int32_t x, int32_t y,
                 int32_t w, int32_t h, uint32_t offset) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    region_t box;
    if (!region(rt, env, x, y, w, h, &box)) return;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_app_addr(inst, offset, (uint32_t)(w * h * 2))) return;
    const uint8_t *pixels = wasm_runtime_addr_app_to_native(inst, offset);
    for (int row = box.top; row < box.bottom; ++row) {
        for (int col = box.left; col < box.right; ++col) {
            const uint8_t *pixel = pixels + ((row - y) * w + col - x) * 2;
            rt->screen[row * rt->width + col] = pixel[0] | ((uint16_t)pixel[1] << 8);
        }
    }
}

/* Encoded zero repeats the preceding row. Index zero is transparent. */
static const char *decode_sprite(const uint8_t *start, size_t bytes, int w, int h,
                                 hachi_runtime_t *rt, const region_t *box, int x, int y) {
    if (!bytes || bytes > HACHI_MAX_SPRITE || w < 1 || w > HACHI_NATIVE_W
        || h < 1 || h > HACHI_NATIVE_H) return "invalid sprite dimensions or byte count";
    unsigned colors = start[0];
    if (!colors || bytes < 1 + 2 * colors) return "invalid sprite palette";
    const uint8_t *p = start + 1 + 2 * colors, *end = start + bytes;
    uint16_t palette[256];
    for (unsigned i = 1; rt && i <= colors; ++i)
        palette[i] = (uint16_t)(start[2 * i - 1] | (start[2 * i] << 8));
    uint8_t previous[HACHI_NATIVE_W] = {0};
    int row = y, column = x, remaining = w * h;
    while (p < end && remaining) {
        unsigned code = *p++;
        int count = (code & 127) + 1;
        bool literal = code < 128;
        if (count > remaining || end - p < (literal ? count : 1)) return "invalid sprite run length";
        for (int i = 0; i < count; ++i) {
            unsigned encoded = p[literal ? i : 0];
            if (encoded > colors + 1) return "invalid sprite palette index";
            unsigned index = encoded ? encoded - 1 : previous[column - x];
            previous[column - x] = (uint8_t)index;
            if (rt && index && row >= box->top && row < box->bottom
                && column >= box->left && column < box->right)
                rt->screen[row * rt->width + column] = palette[index];
            if (++column == x + w) {
                column = x;
                ++row;
            }
        }
        p += literal ? count : 1;
        remaining -= count;
    }
    return remaining || p != end ? "invalid sprite pixel or byte count" : NULL;
}

const char *hachi_sprite_error(const uint8_t *bytes, size_t size, int width, int height) {
    return decode_sprite(bytes, size, width, height, NULL, NULL, 0, 0);
}

static void sprite(wasm_exec_env_t env, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t offset, uint32_t bytes) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    region_t box;
    if (!region(rt, env, x, y, w, h, &box)) return;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!bytes || bytes > HACHI_MAX_SPRITE || !wasm_runtime_validate_app_addr(inst, offset, bytes)) {
        wasm_runtime_set_exception(inst, "invalid sprite address or byte count");
        return;
    }
    const char *error = decode_sprite(wasm_runtime_addr_app_to_native(inst, offset),
                                      bytes, w, h, rt, &box, x, y);
    if (error) wasm_runtime_set_exception(inst, error);
}

static void canvas640(wasm_exec_env_t env) { (void)env; }

/* Decode only scalar UTF-8 and single-line printable text. Count the actual glyph budget. */
static unsigned text_length(const uint8_t *bytes, unsigned length) {
    unsigned glyphs = 0;
    for (unsigned i = 0; i < length; ++glyphs) {
        unsigned lead = bytes[i++];
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7f) return 0;
            continue;
        }
        unsigned extra = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if (lead < 0xc2 || lead > 0xf4 || extra > length - i) return 0;
        unsigned cp = lead & (0x7f >> (extra + 1));
        for (unsigned j = 0; j < extra; ++j) {
            unsigned tail = bytes[i++];
            if ((tail & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (tail & 63);
        }
        if (cp < (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return 0;
    }
    return glyphs;
}

static void text(wasm_exec_env_t env, int32_t x, int32_t y, int32_t px,
                 int32_t rgb, uint32_t address, uint32_t length) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!rt->drawing_frame || rt->width != HACHI_NATIVE_W || px < 12 || px > 48 ||
        x < 0 || x >= rt->width || y < 0 || y >= rt->height ||
        !length || length > HACHI_TEXT_BYTES ||
        !wasm_runtime_validate_app_addr(inst, address, length)) goto invalid;
    const uint8_t *source = wasm_runtime_addr_app_to_native(inst, address);
    unsigned glyphs = text_length(source, length);
    if (!glyphs) goto invalid;
    unsigned work = glyphs * (unsigned)(px * px);
    if (rt->text.count >= HACHI_TEXT_LIMIT || work > HACHI_TEXT_WORK - rt->text_work) goto invalid;
    rt->text_work += work;
    hachi_text_t *item = &rt->text.items[rt->text.count++];
    item->x = (uint16_t)x;
    item->y = (uint16_t)y;
    item->px = (uint16_t)px;
    unsigned color = (unsigned)rgb;
    item->color = (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31));
    memcpy(item->utf8, source, length);
    item->utf8[length] = 0;
    return;
invalid:
    wasm_runtime_set_exception(inst, "invalid text or text budget exceeded");
}

static void tone(wasm_exec_env_t env, int32_t hz, int32_t ms, int32_t volume) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    if (hz < 40 || hz > 8000 || ms < 1 || ms > 2000 || volume < 0 || volume > 100) return;
    if (rt->tone_count >= 8) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "audio event limit");
        return;
    }
    rt->tones[rt->tone_count++] = (hachi_tone_t){hz, ms, volume};
}

#if HACHI_AUDIO_ENABLED
static void sample(wasm_exec_env_t env, int32_t index) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    bool music = index >= HACHI_MUSIC_FIRST;
    int *request = music ? &rt->music_index : &rt->sample_index;
    if (!rt->sample_event || *request != HACHI_SAMPLE_NONE ||
        index < HACHI_SAMPLE_STOP || index > HACHI_MUSIC_STOP) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "invalid sample call");
        return;
    }
    *request = music ? (index == HACHI_MUSIC_STOP ? HACHI_SAMPLE_STOP : index - HACHI_MUSIC_FIRST) : index;
}
#endif

static int32_t load(wasm_exec_env_t env, int32_t slot, int32_t fallback) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    if (slot < 0 || slot >= 8 || !(rt->saves->present & (1u << slot))) return fallback;
    return rt->saves->values[slot];
}

static void save(wasm_exec_env_t env, int32_t slot, int32_t value) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    if (slot < 0 || slot >= 8) return;
    if (++rt->writes > 8) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "save write limit");
        return;
    }
    rt->saves->values[slot] = value;
    rt->saves->present |= 1u << slot;
}

static void media_cmd(wasm_exec_env_t env, int32_t command) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    if (rt->media_allowed && command >= 1 && command <= 7) rt->media_command = command;
}

static int32_t read_snapshot(wasm_exec_env_t env, int kind, uint32_t offset) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    hachi_snapshot_t snapshot = {0};
    _Static_assert(sizeof(snapshot) == 16, "WASM host snapshot layout");
    if (!rt->drawing_frame || !wasm_runtime_validate_app_addr(inst, offset, sizeof(snapshot))) {
        wasm_runtime_set_exception(inst, "invalid host snapshot call");
        return HACHI_HOST_UNAVAILABLE;
    }
    int status = rt->host_read && kind >= HACHI_HOST_LOCAL && kind <= HACHI_HOST_UV
        ? rt->host_read(kind, &snapshot) : HACHI_HOST_UNAVAILABLE;
    memcpy(wasm_runtime_addr_app_to_native(inst, offset), &snapshot, sizeof(snapshot));
    return status;
}

static int32_t clock_read(wasm_exec_env_t env, int32_t kind, uint32_t offset) {
    return read_snapshot(env, kind == HACHI_HOST_LOCAL || kind == HACHI_HOST_MONOTONIC ? kind : -1,
                         offset);
}

static int32_t uv_read(wasm_exec_env_t env, uint32_t offset) {
    return read_snapshot(env, HACHI_HOST_UV, offset);
}

static uint32_t random_u32(wasm_exec_env_t env) {
    hachi_runtime_t *rt = wasm_runtime_get_user_data(env);
    return rt->host_random ? rt->host_random() : 0;
}

static NativeSymbol imports[] = {
    {"canvas640", (void *)canvas640, "()", NULL},
    {"text", (void *)text, "(iiiiii)", NULL},
    {"rect", (void *)rect, "(iiiii)", NULL},
    {"blit", (void *)blit, "(iiiii)", NULL},
    {"sprite", (void *)sprite, "(iiiiii)", NULL},
    {"tone", (void *)tone, "(iii)", NULL},
    {"load", (void *)load, "(ii)i", NULL},
    {"save", (void *)save, "(ii)", NULL},
    {"clock_read", (void *)clock_read, "(ii)i", NULL},
    {"uv_read", (void *)uv_read, "(i)i", NULL},
    {"random_u32", (void *)random_u32, "()i", NULL},
    {"media_cmd", (void *)media_cmd, "(i)", NULL},
#if HACHI_AUDIO_ENABLED
    {"sample", (void *)sample, "(i)", NULL},
#endif
};

bool hachi_runtime_init(void *pool, uint32_t size) {
    RuntimeInitArgs args = {0};
    args.mem_alloc_type = Alloc_With_Pool;
    args.mem_alloc_option.pool.heap_buf = pool;
    args.mem_alloc_option.pool.heap_size = size;
    args.native_module_name = "hachi";
    args.native_symbols = imports;
    args.n_native_symbols = sizeof(imports) / sizeof(imports[0]);
    return wasm_runtime_full_init(&args);
}

static bool valid_imports(wasm_module_t module) {
    int32_t count = wasm_runtime_get_import_count(module);
    if (count < 0 || count > (int32_t)(sizeof(imports) / sizeof(imports[0]))) return false;
    for (int32_t i = 0; i < count; ++i) {
        wasm_import_t entry = {0};
        wasm_runtime_get_import_type(module, i, &entry);
        if (entry.kind != WASM_IMPORT_EXPORT_KIND_FUNC || !entry.linked ||
            !entry.module_name || strcmp(entry.module_name, "hachi") != 0 ||
            !entry.name) return false;
        bool allowed = false;
        for (size_t j = 0; j < sizeof(imports) / sizeof(imports[0]); ++j) {
            if (strcmp(entry.name, imports[j].symbol) == 0) { allowed = true; break; }
        }
        if (!allowed) return false;
    }
    return true;
}

static bool valid_exports(wasm_module_t module) {
    if (wasm_runtime_get_export_count(module) != 4) return false;
    unsigned seen = 0;
    for (int32_t i = 0; i < 4; ++i) {
        wasm_export_t item = {0};
        wasm_runtime_get_export_type(module, i, &item);
        if (!item.name) return false;
        if (item.kind == WASM_IMPORT_EXPORT_KIND_MEMORY && strcmp(item.name, "memory") == 0)
            seen |= 1;
        else if (item.kind == WASM_IMPORT_EXPORT_KIND_FUNC && strcmp(item.name, "hachi_init") == 0)
            seen |= 2;
        else if (item.kind == WASM_IMPORT_EXPORT_KIND_FUNC && strcmp(item.name, "hachi_event") == 0)
            seen |= 4;
        else if (item.kind == WASM_IMPORT_EXPORT_KIND_FUNC && strcmp(item.name, "hachi_frame") == 0)
            seen |= 8;
        else
            return false;
    }
    return seen == 15;
}

static wasm_function_inst_t entry(hachi_runtime_t *rt, const char *name, uint32_t argc) {
    wasm_function_inst_t fn = wasm_runtime_lookup_function(rt->instance, name);
    if (!fn || wasm_func_get_param_count(fn, rt->instance) != argc ||
        wasm_func_get_result_count(fn, rt->instance) != 0) return NULL;
    wasm_valkind_t types[3];
    if (argc) wasm_func_get_param_types(fn, rt->instance, types);
    for (uint32_t i = 0; i < argc; ++i) if (types[i] != WASM_I32) return NULL;
    return fn;
}

static bool run(hachi_runtime_t *rt, wasm_function_inst_t fn, uint32_t count, uint32_t *args) {
    hachi_saves_t original = *rt->saves;
    rt->tone_count = rt->operations = rt->area = rt->writes = 0;
    rt->media_command = 0;
    rt->sample_index = rt->music_index = HACHI_SAMPLE_NONE;
    wasm_runtime_set_instruction_count_limit(rt->env, 160000);
    if (wasm_runtime_call_wasm(rt->env, fn, count, args)) return true;
    *rt->saves = original;
    rt->tone_count = 0;
    rt->media_command = 0;
    rt->sample_index = rt->music_index = HACHI_SAMPLE_NONE;
    return false;
}

bool hachi_open(hachi_runtime_t *rt, uint8_t *wasm, uint32_t size, uint16_t *screen,
                size_t screen_pixels, hachi_saves_t *saves, char *error, size_t error_size) {
    return hachi_open_host(rt, wasm, size, screen, screen_pixels, saves, NULL, NULL, error, error_size);
}

bool hachi_open_host(hachi_runtime_t *rt, uint8_t *wasm, uint32_t size, uint16_t *screen,
                     size_t screen_pixels, hachi_saves_t *saves, hachi_host_read_fn read,
                     uint32_t (*random)(void), char *error, size_t error_size) {
    *rt = (hachi_runtime_t){.host_read = read, .host_random = random, .screen = screen, .saves = saves};
    bool native;
    if (!hachi_module_profile(wasm, size, &native)) {
        snprintf(error, error_size, "invalid module profile");
        return false;
    }
    rt->width = native ? HACHI_NATIVE_W : HACHI_W;
    rt->height = native ? HACHI_NATIVE_H : HACHI_H;
    if (screen_pixels < (size_t)rt->width * rt->height) {
        snprintf(error, error_size, "framebuffer too small");
        return false;
    }
    rt->module = wasm_runtime_load(wasm, size, error, (uint32_t)error_size);
    if (!rt->module) goto fail;
    if (!valid_imports(rt->module) || !valid_exports(rt->module)) {
        snprintf(error, error_size, "invalid guest imports or exports");
        goto fail;
    }
    rt->instance = wasm_runtime_instantiate(rt->module, 16 * 1024, 0, error, (uint32_t)error_size);
    if (!rt->instance) goto fail;
    rt->env = wasm_runtime_create_exec_env(rt->instance, 16 * 1024);
    if (!rt->env) {
        snprintf(error, error_size, "out of execution stack memory");
        goto fail;
    }
    wasm_runtime_set_user_data(rt->env, rt);
    rt->init = entry(rt, "hachi_init", 0);
    rt->event = entry(rt, "hachi_event", 3);
    rt->frame = entry(rt, "hachi_frame", 1);
    if (!rt->init || !rt->event || !rt->frame) {
        snprintf(error, error_size, "invalid guest exports");
        goto fail;
    }
    memset(screen, 0, (size_t)rt->width * rt->height * sizeof(*screen));
    if (run(rt, rt->init, 0, NULL)) return true;
    snprintf(error, error_size, "%s", wasm_runtime_get_exception(rt->instance));
fail:
    hachi_close(rt);
    return false;
}

bool hachi_event_run(hachi_runtime_t *rt, int type, int a, int b) {
    uint32_t args[] = {(uint32_t)type, (uint32_t)a, (uint32_t)b};
    /* Only completed gestures or explicit presses may request a clip. */
    rt->sample_event = type == 1 || type == 3 || type == 4;
    bool ok = run(rt, rt->event, 3, args);
    rt->sample_event = false;
    return ok;
}

bool hachi_frame_run(hachi_runtime_t *rt, int elapsed_ms) {
    uint32_t args[] = {(uint32_t)elapsed_ms};
    rt->text.count = rt->text_work = 0;
    if (rt->width == HACHI_NATIVE_W)
        memset(rt->screen, 0, (size_t)rt->width * rt->height * sizeof(*rt->screen));
    rt->drawing_frame = true;
    rt->sample_event = true;
    bool ok = run(rt, rt->frame, 1, args);
    rt->sample_event = false;
    rt->drawing_frame = false;
    if (!ok) rt->text.count = 0;
    return ok;
}

void hachi_close(hachi_runtime_t *rt) {
    if (rt->env) wasm_runtime_destroy_exec_env(rt->env);
    if (rt->instance) wasm_runtime_deinstantiate(rt->instance);
    if (rt->module) wasm_runtime_unload(rt->module);
    *rt = (hachi_runtime_t){0};
}

uint32_t hachi_screen_hash(const uint16_t *screen) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < HACHI_W * HACHI_H; ++i) {
        hash = (hash ^ (screen[i] & 255u)) * 16777619u;
        hash = (hash ^ (screen[i] >> 8)) * 16777619u;
    }
    return hash;
}
