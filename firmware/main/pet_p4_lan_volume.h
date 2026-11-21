#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PET_LAN_VOLUME_ID_SIZE 16
#define PET_LAN_VOLUME_WINDOW_MS 45000

typedef enum {
  PET_LAN_VOLUME_NONE, PET_LAN_VOLUME_ARMED, PET_LAN_VOLUME_PENDING,
  PET_LAN_VOLUME_APPROVED, PET_LAN_VOLUME_RUNNING, PET_LAN_VOLUME_APPLIED,
  PET_LAN_VOLUME_FAILED, PET_LAN_VOLUME_CANCELLED,
} pet_lan_volume_phase_t;

typedef struct {
  pet_lan_volume_phase_t phase;
  uint64_t expires;
  uint8_t id[PET_LAN_VOLUME_ID_SIZE];
  uint8_t volume;
} pet_lan_volume_t;

static inline void pet_lan_volume_cancel(pet_lan_volume_t *control, uint64_t now) {
  if (control->phase == PET_LAN_VOLUME_APPROVED) {
    control->phase = PET_LAN_VOLUME_CANCELLED;
    control->expires = now + PET_LAN_VOLUME_WINDOW_MS;
  }
}

static inline void pet_lan_volume_refresh(pet_lan_volume_t *control, uint64_t now) {
  if (control->phase == PET_LAN_VOLUME_RUNNING || now < control->expires) return;
  if (control->phase == PET_LAN_VOLUME_APPROVED) pet_lan_volume_cancel(control, now);
  else *control = (pet_lan_volume_t){0};
}

static inline bool pet_lan_volume_submit(pet_lan_volume_t *control, uint64_t now,
                                        bool trusted, const uint8_t *id, uint8_t volume) {
  pet_lan_volume_refresh(control, now);
  if (!trusted || volume > 100 || control->phase == PET_LAN_VOLUME_APPROVED ||
      control->phase == PET_LAN_VOLUME_RUNNING ||
      (control->phase != PET_LAN_VOLUME_NONE && !memcmp(control->id, id, sizeof(control->id)))) return false;
  memcpy(control->id, id, sizeof(control->id));
  control->volume = volume;
  control->expires = now + PET_LAN_VOLUME_WINDOW_MS;
  control->phase = PET_LAN_VOLUME_APPROVED;
  return true;
}

static inline bool pet_lan_volume_claim(pet_lan_volume_t *control, uint64_t now, uint8_t *volume) {
  pet_lan_volume_refresh(control, now);
  if (control->phase != PET_LAN_VOLUME_APPROVED) return false;
  *volume = control->volume;
  control->phase = PET_LAN_VOLUME_RUNNING;
  return true;
}

static inline pet_lan_volume_phase_t pet_lan_volume_query(pet_lan_volume_t *control, uint64_t now,
                                                        const uint8_t *id) {
  pet_lan_volume_refresh(control, now);
  return control->phase >= PET_LAN_VOLUME_PENDING && !memcmp(control->id, id, sizeof(control->id))
    ? control->phase : PET_LAN_VOLUME_NONE;
}
