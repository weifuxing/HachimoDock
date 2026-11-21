#pragma once

#include "cJSON.h"
#include "pet_p4_json_source.h"

#define PET_P4_COMPONENT_METADATA_BYTES 8192

typedef struct {
  char version[48];
  char kind[5];
} pet_p4_component_metadata_t;

/* Metadata is advisory. Older catalogs and out-of-range fields remain usable
 * with unknown inventory values. Never truncate a version into a different ID. */
static inline pet_p4_component_metadata_t pet_p4_component_metadata_fields(
    const cJSON *version, const cJSON *kind) {
  pet_p4_component_metadata_t out = {0};
  if (cJSON_IsString(version)) {
    const char *value = version->valuestring;
    const size_t length = strlen(value);
    bool safe = length > 0 && length < sizeof(out.version);
    for (size_t i = 0; safe && i < length; ++i)
      safe = (unsigned char)value[i] >= 32 && (unsigned char)value[i] != 127;
    if (safe) memcpy(out.version, value, length);
  }
  if (cJSON_IsString(kind) &&
      (!strcmp(kind->valuestring, "game") || !strcmp(kind->valuestring, "tool")))
    memcpy(out.kind, kind->valuestring, sizeof(out.kind));
  return out;
}

/* The caller has collected exactly one digest-covered file plus its NUL. */
static inline pet_p4_component_metadata_t pet_p4_component_metadata_read(
    const char *text, size_t size, const char *expected_id) {
  pet_p4_component_metadata_t out = {0};
  if (!size || size > PET_P4_COMPONENT_METADATA_BYTES ||
      !pet_json_source_bounded_text_bytes(text, size)) return out;
  cJSON *root = cJSON_ParseWithLengthOpts(text, size + 1, NULL, true);
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
  if (cJSON_IsObject(root) && cJSON_IsString(id) && !strcmp(id->valuestring, expected_id))
    out = pet_p4_component_metadata_fields(
      cJSON_GetObjectItemCaseSensitive(root, "version"),
      cJSON_GetObjectItemCaseSensitive(root, "kind"));
  cJSON_Delete(root);
  return out;
}
