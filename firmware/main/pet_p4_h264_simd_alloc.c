/* Only libtinyh264's allocation references are redirected here by objcopy.
 * P4 RTC/TCM can satisfy INTERNAL but cannot serve 128-bit SIMD accesses.
 * Preserve every vendor preference, adding SIMD even to fallback requests.
 * Free remains heap_caps_free/free: these are ordinary IDF allocations.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_heap_caps.h"

/* The inspected 1.5.0 archive uses at most two preferences. Allow three,
 * retaining IDF's single failure notification after all preferences fail.
 * Unknown future ABI use fails closed rather than allocating unsafe memory.
 */
static int simd_caps(size_t num, va_list args, uint32_t caps[3]) {
    if (num == 0 || num > 3) return 0;
    for (size_t i = 0; i < num; ++i)
        caps[i] = va_arg(args, uint32_t) | MALLOC_CAP_SIMD;
    return 1;
}

void *pet_h264_simd_malloc_prefer(size_t size, size_t num, ...) {
    uint32_t caps[3] = {0};
    va_list args;
    va_start(args, num);
    int valid = simd_caps(num, args, caps);
    va_end(args);
    if (!valid) return NULL;
    return heap_caps_malloc_prefer(size, num, caps[0], caps[1], caps[2]);
}

void *pet_h264_simd_calloc_prefer(size_t n, size_t size, size_t num, ...) {
    if (size && n > SIZE_MAX / size) return NULL;
    uint32_t caps[3] = {0};
    va_list args;
    va_start(args, num);
    int valid = simd_caps(num, args, caps);
    va_end(args);
    if (!valid) return NULL;
    return heap_caps_calloc_prefer(n, size, num, caps[0], caps[1], caps[2]);
}

void *pet_h264_simd_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_SIMD);
}
