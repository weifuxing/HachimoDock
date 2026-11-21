#include "hachi_runtime.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t binary[HACHI_MAX_MODULE + 1], pool[HACHI_POOL_BYTES];
static uint16_t screen[HACHI_NATIVE_W * HACHI_NATIVE_H];
static uint64_t monotonic_ms;
static hachi_snapshot_t local_clock, uv;
static int uv_status;
static uint16_t observed_pixels[HACHI_NATIVE_W * HACHI_NATIVE_H];
static hachi_text_layer_t observed_text;

static int read_host(int kind, hachi_snapshot_t *out) {
    if (kind == HACHI_HOST_LOCAL && local_clock.time_ms) {
        *out = local_clock;
        out->time_ms += monotonic_ms;
        return HACHI_HOST_READY;
    }
    if (kind == HACHI_HOST_UV && uv_status) {
        *out = uv;
        return uv_status;
    }
    if (kind != HACHI_HOST_MONOTONIC) return HACHI_HOST_UNAVAILABLE;
    out->time_ms = monotonic_ms;
    return HACHI_HOST_READY;
}

static uint32_t random_state = 0x73a193e4;
static uint32_t random_word(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static bool draw(hachi_runtime_t *runtime, int elapsed) {
    monotonic_ms += (unsigned)elapsed;
    return hachi_frame_run(runtime, elapsed > 80 ? 80 : elapsed);
}

static bool contains_text(const hachi_text_layer_t *layer, const char *text) {
    for (unsigned i = 0; i < layer->count; ++i)
        if (!strcmp(layer->items[i].utf8, text)) return true;
    return false;
}

static uint32_t visible_hash(const hachi_runtime_t *runtime) {
    uint32_t hash = 2166136261u;
    const unsigned count = (unsigned)(runtime->width * runtime->height);
    for (unsigned i = 0; i < count; ++i)
        hash = (hash ^ screen[i]) * 16777619u;
    for (unsigned i = 0; i < runtime->text.count; ++i) {
        const hachi_text_t *item = &runtime->text.items[i];
        hash = (hash ^ item->x) * 16777619u;
        hash = (hash ^ item->y) * 16777619u;
        hash = (hash ^ item->px) * 16777619u;
        hash = (hash ^ item->color) * 16777619u;
        for (const uint8_t *byte = (const uint8_t *)item->utf8; *byte; ++byte)
            hash = (hash ^ *byte) * 16777619u;
        hash = (hash ^ 255u) * 16777619u;
    }
    return hash;
}

static bool run_trace(hachi_runtime_t *runtime, FILE *trace, unsigned *step) {
    if (!trace) {
        for (; *step < 120; ++*step)
            if (!draw(runtime, 33)) return false;
        /* Imported/precompiled modules receive the same bounded input probe.
         * No gameplay assertion: a legal no-op is compatible. Traps are not. */
        for (int key = 1; key <= 6; ++key) {
            if (!hachi_event_run(runtime, 4, key, 0) || !draw(runtime, 33)) return false;
            ++*step;
        }
        for(int x=runtime->width/4;x<runtime->width;x+=runtime->width/4){
            if(!hachi_event_run(runtime,1,x,runtime->height/2)||!draw(runtime,33)||
               !hachi_event_run(runtime,3,x,runtime->height/2)||!draw(runtime,33))return false;
            *step+=2;
        }
        return true;
    }
    char line[192];
    uint32_t observed = 0;
    bool have_frame = false, have_observation = false;
    hachi_saves_t observed_saves = {0};
    while (fgets(line, sizeof(line), trace)) {
        ++*step;
        if (*step > 50000) return false;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        int a, b, c;
        char extra;
        unsigned color;
        unsigned long long timestamp;
        if (sscanf(line, "clock %llu %d %c", &timestamp, &a, &extra) == 2
            && timestamp <= 4102444800000ULL && a >= -840 && a <= 840) {
            local_clock = (hachi_snapshot_t){.time_ms = timestamp, .value = a};
        } else if (sscanf(line, "uv %d %d %d %c", &a, &b, &c, &extra) == 3
                   && a >= 0 && a <= 2 && b >= 0 && b <= 2500 && c >= 0
                   && (uint64_t)c <= local_clock.time_ms + monotonic_ms) {
            uv_status = a;
            uv = (hachi_snapshot_t){.time_ms = local_clock.time_ms + monotonic_ms - (unsigned)c,
                                   .value = b, .age_ms = (uint32_t)c};
        } else if (sscanf(line, "frame %d %c", &a, &extra) == 1 && a >= 0 && a <= 10000) {
            if (!draw(runtime, a)) return false;
            have_frame = true;
        } else if (sscanf(line, "event %d %d %d %c", &a, &b, &c, &extra) == 3
                   && a >= 1 && a <= 6 && b >= 0 && b <= 640 && c >= 0 && c <= 480) {
            if (!hachi_event_run(runtime, a, b, c)) return false;
        } else if (sscanf(line, "sample %d %c", &a, &extra) == 1
                   && a >= HACHI_SAMPLE_NONE && a < HACHI_SAMPLE_MAX) {
            if (runtime->sample_index != a) {
                fprintf(stderr, "expected sample %d, found %d\n", a, runtime->sample_index);
                return false;
            }
        } else if (!strcmp(line, "observe") && have_frame) {
            observed = visible_hash(runtime);
            memcpy(observed_pixels, screen, (size_t)runtime->width * runtime->height * sizeof(*screen));
            observed_text = runtime->text;
            observed_saves = *runtime->saves;
            have_observation = true;
        } else if ((!strcmp(line, "changed") || !strcmp(line, "unchanged")) && have_observation) {
            bool changed = visible_hash(runtime) != observed;
            if (changed != !strcmp(line, "changed")) {
                fprintf(stderr, "expected frame %s, but found %s\n",
                        line, changed ? "changed" : "unchanged");
                return false;
            }
        } else if (!strncmp(line, "appeared ", 9) && have_observation) {
            if (contains_text(&observed_text, line + 9) || !contains_text(&runtime->text, line + 9)) {
                fprintf(stderr, "expected newly visible text: %s\n", line + 9);
                return false;
            }
        } else if (sscanf(line, "pixel-changed %d %d %x %c", &a, &b, &color, &extra) == 3
                   && have_observation && a >= 0 && a < runtime->width
                   && b >= 0 && b < runtime->height && color <= UINT16_MAX) {
            size_t pixel = (size_t)b * runtime->width + a;
            if (screen[pixel] != color || observed_pixels[pixel] == color) {
                fprintf(stderr, "expected pixel (%d, %d) to change to %04x\n", a, b, color);
                return false;
            }
        } else if (sscanf(line, "slot-changed %d %d %c", &a, &b, &extra) == 2
                   && have_observation && a >= 0 && a < 8) {
            unsigned bit = 1u << a;
            if (!(runtime->saves->present & bit) || runtime->saves->values[a] != b
                || ((observed_saves.present & bit) && observed_saves.values[a] == b)) {
                fprintf(stderr, "expected save slot %d to change to %d\n", a, b);
                return false;
            }
        } else if (!strncmp(line, "text ", 5)) {
            if (!contains_text(&runtime->text, line + 5)) {
                fprintf(stderr, "expected visible text: %s\n", line + 5);
                return false;
            }
        } else {
            fprintf(stderr, "invalid trace instruction: %s\n", line);
            return false;
        }
    }
    return !ferror(trace) && *step > 0;
}

/* Interactive preview uses the identical bounded interpreter as the device. */
static void json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
        if (*p=='"' || *p=='\\') { putchar('\\'); putchar(*p); }
        else if (*p<32) printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static void reply(hachi_runtime_t *rt, bool frame) {
    printf("{\"width\":%d,\"height\":%d,\"frame\":%s,\"pixels\":[],\"texts\":[",rt->width,rt->height,frame?"true":"false");
    for(unsigned i=0;frame && i<rt->text.count;++i) {
        hachi_text_t *t=&rt->text.items[i];
        unsigned r=t->color>>11,g=(t->color>>5)&63,b=t->color&31;
        unsigned rgb=(((r<<3)|(r>>2))<<16)|(((g<<2)|(g>>4))<<8)|((b<<3)|(b>>2));
        if(i)putchar(',');
        printf("{\"x\":%u,\"y\":%u,\"px\":%u,\"rgb\":%u,\"value\":",t->x,t->y,t->px,rgb);
        json_string(t->utf8);putchar('}');
    }
    printf("],\"tones\":[");
    for(unsigned i=0;i<rt->tone_count;++i){if(i)putchar(',');printf("{\"hz\":%d,\"ms\":%d,\"volume\":%d}",rt->tones[i].hz,rt->tones[i].ms,rt->tones[i].volume);}
    printf("],\"cue\":%d,\"music\":%d,\"runs\":[", rt->sample_index, rt->music_index);
    if(frame)for(unsigned i=0,n=rt->width*rt->height;i<n;){unsigned count=1;while(i+count<n && rt->screen[i+count]==rt->screen[i])++count;if(i)putchar(',');printf("%u,%u",count,rt->screen[i]);i+=count;}
    printf("]}\n");fflush(stdout);
}
static bool serve(hachi_runtime_t *rt) {
    reply(rt,false);
    char line[192],extra;
    while(fgets(line,sizeof(line),stdin)) {
        int a,b,c; unsigned long long epoch;
        bool frame=false,ok=true;
        if(sscanf(line,"clock %llu %d %c",&epoch,&a,&extra)==2 && a>=-840&&a<=840) {
            local_clock=(hachi_snapshot_t){.time_ms=epoch?epoch-monotonic_ms:0,.value=a};
        } else if(sscanf(line,"frame %d %c",&a,&extra)==1&&a>=0&&a<=80){frame=true;ok=draw(rt,a);}
        else if(sscanf(line,"event %d %d %d %c",&a,&b,&c,&extra)==3&&a>=1&&a<=6&&b>=0&&b<=640&&c>=0&&c<=480)ok=hachi_event_run(rt,a,b,c);
        else return false;
        if(!ok){printf("{\"error\":");json_string(wasm_runtime_get_exception(rt->instance));printf("}\n");fflush(stdout);return false;}
        reply(rt,frame);
        if(frame && rt->width==HACHI_W)rt->screen=rt->screen==screen?observed_pixels:screen;
    }
    return true;
}

int main(int argc, char **argv) {
    const char *seed=getenv("PETUI_TEST_SEED");
    if(seed)random_state=(uint32_t)strtoul(seed,NULL,0)|1u;
    if (argc == 5 && !strcmp(argv[1], "--sprite")) {
        int width, height;
        char extra;
        if (sscanf(argv[2], "%d%c", &width, &extra) != 1 ||
            sscanf(argv[3], "%d%c", &height, &extra) != 1) return 2;
        FILE *file = fopen(argv[4], "rb");
        if (!file) return 2;
        size_t size = fread(binary, 1, HACHI_MAX_SPRITE + 1, file);
        fclose(file);
        const char *error = hachi_sprite_error(binary, size, width, height);
        if (error) fprintf(stderr, "%s: %s\n", argv[4], error);
        return error ? 1 : 0;
    }
    const bool interactive = argc == 3 && !strcmp(argv[2], "--serve");
    const bool first = argc == 3 && !strcmp(argv[2], "--first-frame");
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: petui-wasm-check app.wasm [trace.txt]\n");
        return 2;
    }
    FILE *module = fopen(argv[1], "rb");
    FILE *trace = argc == 3 && !first && !interactive ? fopen(argv[2], "r") : NULL;
    if (!module || (argc == 3 && !first && !interactive && !trace)) {
        if (module) fclose(module);
        if (trace) fclose(trace);
        fprintf(stderr, "cannot read module or trace\n");
        return 2;
    }
    const size_t size = fread(binary, 1, sizeof(binary), module);
    fclose(module);
    hachi_runtime_t runtime = {0};
    hachi_saves_t saves = {0};
    if (trace) {
        char line[192], extra;
        int slot, value;
        long start = ftell(trace);
        while (fgets(line, sizeof(line), trace) && !strncmp(line, "seed-save ", 10)) {
            if (sscanf(line, "seed-save %d %d %c", &slot, &value, &extra) != 2 || slot < 0 || slot >= 8) {
                fprintf(stderr, "invalid seed-save instruction\n");
                fclose(trace);
                return 2;
            }
            saves.present |= 1u << slot;
            saves.values[slot] = value;
            start = ftell(trace);
        }
        if (start < 0 || fseek(trace, start, SEEK_SET)) {
            fclose(trace);
            return 2;
        }
    }
    if (interactive) {
        char line[192],extra; unsigned long long epoch; int offset;
        if(!fgets(line,sizeof(line),stdin)||sscanf(line,"clock %llu %d %c",&epoch,&offset,&extra)!=2) return 2;
        local_clock=(hachi_snapshot_t){.time_ms=epoch,.value=offset};
    }
    char error[256] = "runtime initialization failed";
    if (!hachi_runtime_init(pool, sizeof(pool)) ||
        !hachi_open_host(&runtime, binary, size, screen, sizeof(screen) / sizeof(*screen),
                         &saves, read_host, random_word, error, sizeof(error))) {
        if (trace) fclose(trace);
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    unsigned step = 0;
    const bool ok = interactive ? serve(&runtime) : first ? (++step, draw(&runtime, 33)) : run_trace(&runtime, trace, &step);
    if (trace) fclose(trace);
    if (ok && !interactive) printf("WAMR: %dx%d, %u steps passed\n", runtime.width, runtime.height, step);
    else if (!ok) {
        const char *exception = wasm_runtime_get_exception(runtime.instance);
        fprintf(stderr, "WAMR step %u: %s\n", step, exception ? exception : "trace failed");
    }
    hachi_close(&runtime);
    return ok ? 0 : 1;
}
