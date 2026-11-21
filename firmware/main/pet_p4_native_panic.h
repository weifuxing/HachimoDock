/* Private, default-off numeric native exception evidence. Not a crash dump,
 * reset authority, recovery mechanism or replacement for the SDK handler. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_NATIVE_PANIC_TRACE
#define PET_P4_NATIVE_PANIC_TRACE 0
#endif
#ifndef PET_P4_H264_PANIC_TRACE
#define PET_P4_H264_PANIC_TRACE 0
#endif
#if PET_P4_H264_PANIC_TRACE && !PET_P4_NATIVE_PANIC_TRACE
#error "H.264 panic evidence requires the native panic observer"
#endif
#if PET_P4_NATIVE_PANIC_TRACE
#define PET_NATIVE_PANIC_STORAGE_BYTES (144U+64U*PET_P4_H264_PANIC_TRACE)
#if PET_P4_H264_PANIC_TRACE
typedef struct {
  uint32_t stream_crc32, stream_bytes, stream_offset, next_frame;
  uint32_t width, height, trap_value, register_a0;
} pet_native_panic_h264_t;
/* The renderer passes a local record around its single decoder call. */
void pet_native_panic_h264_begin(const pet_native_panic_h264_t *decode);
void pet_native_panic_h264_end(void);
#endif
typedef struct {
  uint32_t pc,return_pc,cause,core,exception,flags;
#if PET_P4_H264_PANIC_TRACE
  pet_native_panic_h264_t h264;
#endif
} pet_native_panic_site_t;
typedef struct {
  bool same_build;
  uint8_t valid_mask;
  pet_native_panic_site_t sites[2];
} pet_native_panic_snapshot_t;
void pet_native_panic_boot(void);
bool pet_native_panic_previous(pet_native_panic_snapshot_t *out);
#endif
