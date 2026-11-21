#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* Byte offsets into the verified, decompressed firmware-owned JSON. This is
 * not a new package format: each slice remains the original package object. */
typedef struct { uint32_t offset, length; } pet_builtin_slice_t;
extern const unsigned char pet_p4_builtin_components_zlib[];
extern const size_t pet_p4_builtin_components_zlib_size;
extern const size_t pet_p4_builtin_components_json_size;
extern const char pet_p4_builtin_index_json[];
extern const pet_builtin_slice_t pet_p4_builtin_slices[];
extern const size_t pet_p4_builtin_slice_count;

static inline cJSON *pet_builtin_parse_slice(const char *raw, size_t size,
    const pet_builtin_slice_t *slice) {
  if (!raw || !slice || !slice->length || slice->length > 32768U
      || slice->offset > size || slice->length > size - slice->offset) return NULL;
  const char *end = NULL;
  cJSON *item = cJSON_ParseWithLengthOpts(raw + slice->offset, slice->length, &end, 0);
  if (!cJSON_IsObject(item) || end != raw + slice->offset + slice->length) {
    cJSON_Delete(item);
    return NULL;
  }
  return item;
}
