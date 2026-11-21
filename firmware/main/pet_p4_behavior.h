#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pet_p4_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char lifecycle[32];
  uint64_t state_since_ms;
  uint64_t welcome_until_ms;
  uint64_t selected_since_ms;
  uint64_t first_video_frame_ms;
  uint64_t video_progress_ms;
  uint32_t asset_revision;
  int selected_index;
  int presented_video_frame;
  bool play_video_once;
  bool initialized;
} pet_p4_behavior_t;

void pet_p4_behavior_init(pet_p4_behavior_t *behavior);
const char *pet_p4_canonical_lifecycle(const char *lifecycle);
int pet_p4_behavior_select(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle,
  uint32_t asset_revision,
  uint64_t now_ms
);
int pet_p4_behavior_select_video(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle,
  uint32_t asset_revision,
  uint64_t now_ms,
  bool allow_touch_tail
);
void pet_p4_behavior_presented_video_frame(
  pet_p4_behavior_t *behavior, int frame, uint64_t now_ms
);
int pet_p4_behavior_video_target_frame(
  const pet_p4_behavior_t *behavior, int frames, uint64_t duration_ms, uint64_t now_ms
);

#ifdef __cplusplus
}
#endif
