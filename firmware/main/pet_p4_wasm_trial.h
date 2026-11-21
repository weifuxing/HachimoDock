#pragma once
#include "cJSON.h"
#include "hachi_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "pet_p4_touch_core.h"

unsigned pet_p4_wasm_package(void *bytes, size_t capacity, size_t *size,
                            unsigned expected, char id[48], bool *radio);
/* A nonzero result belongs to this exact active component runtime revision. */
uint32_t pet_p4_wasm_speech_generation(unsigned revision);

bool pet_p4_wasm_validate_install(const char *id, const uint8_t *module, size_t size,
                                  bool radio, char *error, size_t error_size);
void pet_p4_wasm_trial_diagnostics(cJSON *parent);
void pet_p4_wasm_trial_tick(bool visible);
bool pet_p4_wasm_trial_pointer_active(void);
bool pet_p4_wasm_trial_pointer(const pet_p4_pointer_event_t *event);
unsigned pet_p4_wasm_action_key(const char *action, bool media);
bool pet_p4_wasm_trial_action(const char *action, bool *accepted);
typedef enum {
  PET_WASM_DRAW_NONE,
  PET_WASM_DRAW_PENDING,
  PET_WASM_DRAW_READY,
  PET_WASM_DRAW_FAILED
} pet_wasm_draw_state_t;
pet_wasm_draw_state_t pet_p4_wasm_trial_draw(
    const char *id, unsigned revision, uint16_t *frame, void (*draw_text)(const hachi_text_t *));
void pet_p4_wasm_trial_tone(int hz, int ms, int volume);

/* Queried and updated on the protocol thread. */
bool pet_p4_wasm_uv_requested(uint64_t now_ms);
void pet_p4_wasm_uv_update(const cJSON *value);
