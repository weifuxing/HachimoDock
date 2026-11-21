#include "pet_p4_behavior.h"

#include <stdio.h>
#include <string.h>

#define PET_P4_IDLE_VARIANT_FALLBACK_MS 6500ULL
#define PET_P4_WORKING_VARIANT_FALLBACK_MS 4800ULL
#define PET_P4_WELCOME_MIN_MS 1200ULL
#define PET_P4_VIDEO_STALL_GRACE_MS 2000ULL

const char *pet_p4_canonical_lifecycle(const char *lifecycle) {
  if (!lifecycle || !lifecycle[0]) return "idle";
  if (!strcmp(lifecycle, "active") || !strcmp(lifecycle, "thinking") || !strcmp(lifecycle, "tool_running")) {
    return "working";
  }
  if (!strcmp(lifecycle, "notification")) return "waiting_user";
  if (!strcmp(lifecycle, "complete") || !strcmp(lifecycle, "completed")) return "done";
  if (!strcmp(lifecycle, "failed")) return "error";
  return lifecycle;
}

static uint64_t asset_duration_ms(
  const pet_p4_asset_entry_t *entry,
  uint64_t fallback_ms
) {
  uint64_t duration = 0;
  if (entry && entry->frames > 0) {
    if (entry->duration_ms > 0) {
      duration = entry->duration_ms;
    } else if (entry->frame_duration_ms > 0) {
      duration = (uint64_t) entry->frames * entry->frame_duration_ms;
    } else if (entry->fps > 0) {
      duration = ((uint64_t) entry->frames * 1000ULL) / entry->fps;
    }
  }
  return duration > 0 ? duration : fallback_ms;
}

static uint64_t welcome_duration_ms(const pet_p4_asset_catalog_t *catalog, int welcome_index) {
  uint64_t duration;
  if (!catalog || welcome_index < 0 || welcome_index >= catalog->count) return 0;
  duration = asset_duration_ms(&catalog->entries[welcome_index], PET_P4_WELCOME_MIN_MS);
  if (duration < PET_P4_WELCOME_MIN_MS) duration = PET_P4_WELCOME_MIN_MS;
  return duration;
}

static int select_tracked_asset(
  pet_p4_behavior_t *behavior,
  int index,
  uint64_t now_ms
) {
  if (!behavior || index < 0) return index;
  if (behavior->selected_index != index) {
    behavior->selected_index = index;
    behavior->selected_since_ms = now_ms;
    behavior->presented_video_frame = -1;
    behavior->first_video_frame_ms = 0;
    behavior->video_progress_ms = now_ms;
  }
  return index;
}

static bool video_finished(
  const pet_p4_behavior_t *behavior, const pet_p4_asset_entry_t *entry,
  uint64_t now_ms, uint64_t duration_ms
) {
  if (entry->frames <= 0) return true;
  if (behavior->presented_video_frame == entry->frames - 1) {
    return now_ms >= behavior->first_video_frame_ms
      && now_ms - behavior->first_video_frame_ms >= duration_ms;
  }
  /* Leave a corrupt or unavailable clip eventually, but keep waiting while
   * the LCD is still receiving new frames, even when decode runs behind. */
  uint64_t frame_ms = (duration_ms + entry->frames - 1) / entry->frames;
  return now_ms >= behavior->video_progress_ms
    && now_ms - behavior->video_progress_ms >= frame_ms + PET_P4_VIDEO_STALL_GRACE_MS;
}

void pet_p4_behavior_presented_video_frame(
  pet_p4_behavior_t *behavior, int frame, uint64_t now_ms
) {
  if (frame == behavior->presented_video_frame
      || (behavior->play_video_once && frame < behavior->presented_video_frame)) return;
  if (behavior->presented_video_frame < 0 || frame < behavior->presented_video_frame) {
    behavior->first_video_frame_ms = now_ms;
  }
  behavior->presented_video_frame = frame;
  behavior->video_progress_ms = now_ms;
}

int pet_p4_behavior_video_target_frame(
  const pet_p4_behavior_t *behavior, int frames, uint64_t duration_ms, uint64_t now_ms
) {
  if (frames <= 1 || duration_ms == 0) return 0;
  int presented = behavior->presented_video_frame;
  if (presented < 0 || now_ms < behavior->first_video_frame_ms) return 0;
  uint64_t elapsed = now_ms - behavior->first_video_frame_ms;
  if (!behavior->play_video_once && presented == frames - 1 && elapsed >= duration_ms) return 0;
  int due = elapsed >= duration_ms ? frames - 1 : (int)(elapsed * (uint64_t)frames / duration_ms);
  /* Advance at most one frame per LCD submission. */
  return due > presented + 1 ? presented + 1 : due;
}

static int rotating_prefix(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *prefix,
  uint64_t now_ms,
  uint64_t fallback_ms,
  uint32_t seed,
  bool video
) {
  int count = pet_p4_asset_catalog_count_prefix(catalog, prefix);
  int position = -1;
  int index = behavior ? behavior->selected_index : -1;
  if (!behavior || count <= 0) return -1;
  behavior->play_video_once = video && count > 1;

  for (int i = 0; i < count; i += 1) {
    if (pet_p4_asset_catalog_nth_prefix(catalog, prefix, i) == index) {
      position = i;
      break;
    }
  }
  if (position < 0) {
    position = (int) (seed % (uint32_t) count);
    index = pet_p4_asset_catalog_nth_prefix(catalog, prefix, position);
    return select_tracked_asset(behavior, index, now_ms);
  }
  if (count == 1) return index;

  if (video) {
    const pet_p4_asset_entry_t *entry = &catalog->entries[index];
    uint64_t duration = asset_duration_ms(entry, fallback_ms);
    if (now_ms < behavior->selected_since_ms || now_ms - behavior->selected_since_ms < duration
        || !video_finished(behavior, entry, now_ms, duration)) return index;
    index = pet_p4_asset_catalog_nth_prefix(catalog, prefix, (position + 1) % count);
    return select_tracked_asset(behavior, index, now_ms);
  }

  uint64_t cycle_duration_ms = 0;
  for (int i = 0; i < count; i += 1) {
    int candidate = pet_p4_asset_catalog_nth_prefix(catalog, prefix, i);
    if (candidate < 0 || candidate >= catalog->count) continue;
    uint64_t duration = asset_duration_ms(&catalog->entries[candidate], fallback_ms);
    if (UINT64_MAX - cycle_duration_ms < duration) {
      cycle_duration_ms = 0;
      break;
    }
    cycle_duration_ms += duration;
  }

  uint64_t elapsed_ms = now_ms >= behavior->selected_since_ms
    ? now_ms - behavior->selected_since_ms : 0;
  if (cycle_duration_ms > 0 && elapsed_ms >= cycle_duration_ms) {
    uint64_t skipped_ms = (elapsed_ms / cycle_duration_ms) * cycle_duration_ms;
    behavior->selected_since_ms += skipped_ms;
    elapsed_ms -= skipped_ms;
  }
  for (int step = 0; step < count; step += 1) {
    uint64_t duration = asset_duration_ms(&catalog->entries[index], fallback_ms);
    if (elapsed_ms < duration) break;
    elapsed_ms -= duration;
    behavior->selected_since_ms += duration;
    position = (position + 1) % count;
    index = pet_p4_asset_catalog_nth_prefix(catalog, prefix, position);
    behavior->selected_index = index;
  }
  return index;
}

static int fallback_idle(const pet_p4_asset_catalog_t *catalog) {
  int index = pet_p4_asset_catalog_find_exact(catalog, "idle.default");
  if (index >= 0) return index;
  index = pet_p4_asset_catalog_nth_prefix(catalog, "idle.", 0);
  if (index >= 0) return index;
  return catalog && catalog->count > 0 ? 0 : -1;
}

void pet_p4_behavior_init(pet_p4_behavior_t *behavior) {
  if (!behavior) return;
  memset(behavior, 0, sizeof(*behavior));
  behavior->selected_index = -1;
  behavior->presented_video_frame = -1;
}

static int select_behavior(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle,
  uint32_t asset_revision,
  uint64_t now_ms,
  bool video,
  bool allow_touch_tail
) {
  const char *canonical = pet_p4_canonical_lifecycle(lifecycle);
  int index;
  if (!behavior || !catalog || catalog->count == 0) return -1;
  behavior->play_video_once = false;

  if (!behavior->initialized) {
    behavior->initialized = true;
    behavior->state_since_ms = now_ms;
    behavior->asset_revision = asset_revision;
    behavior->selected_index = -1;
    snprintf(behavior->lifecycle, sizeof(behavior->lifecycle), "%s", canonical);
    if (!strcmp(canonical, "idle") || !strcmp(canonical, "welcome")) {
      index = pet_p4_asset_catalog_find_exact(catalog, "welcome");
      if (index >= 0) behavior->welcome_until_ms = now_ms + welcome_duration_ms(catalog, index);
    }
  } else if (behavior->asset_revision != asset_revision) {
    behavior->asset_revision = asset_revision;
    behavior->selected_index = -1;
    behavior->welcome_until_ms = 0;
    if (!strcmp(canonical, "idle") || !strcmp(canonical, "welcome")) {
      index = pet_p4_asset_catalog_find_exact(catalog, "welcome");
      if (index >= 0) behavior->welcome_until_ms = now_ms + welcome_duration_ms(catalog, index);
    }
  }

  if (video && allow_touch_tail && !strcmp(canonical, "idle")
      && !strncmp(behavior->lifecycle, "touch.", 6)
      && behavior->selected_index >= 0 && behavior->selected_index < catalog->count) {
    const pet_p4_asset_entry_t *entry = &catalog->entries[behavior->selected_index];
    if (!strncmp(entry->family, "touch.", 6)
        && !video_finished(behavior, entry, now_ms, asset_duration_ms(entry, PET_P4_WELCOME_MIN_MS))) {
      behavior->play_video_once = true;
      return behavior->selected_index;
    }
  }
  if (strcmp(behavior->lifecycle, canonical) != 0) {
    snprintf(behavior->lifecycle, sizeof(behavior->lifecycle), "%s", canonical);
    behavior->state_since_ms = now_ms;
    behavior->selected_index = -1;
    behavior->welcome_until_ms = 0;
  }
  if (behavior->welcome_until_ms && (!strcmp(canonical, "idle") || !strcmp(canonical, "welcome"))) {
    index = pet_p4_asset_catalog_find_exact(catalog, "welcome");
    bool one_shot = video && !strcmp(canonical, "idle");
    if (index >= 0 && (now_ms < behavior->welcome_until_ms
        || (one_shot && behavior->selected_index == index
            && !video_finished(behavior, &catalog->entries[index], now_ms,
                               asset_duration_ms(&catalog->entries[index], PET_P4_WELCOME_MIN_MS))))) {
      behavior->play_video_once = one_shot;
      return select_tracked_asset(behavior, index, now_ms);
    }
    behavior->welcome_until_ms = 0;
  }

  if (!strcmp(canonical, "welcome") || !strcmp(canonical, "waiting_user")
      || !strcmp(canonical, "done") || !strcmp(canonical, "error")) {
    index = pet_p4_asset_catalog_find_exact(catalog, canonical);
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
  }
  if (!strncmp(canonical, "touch.", 6)) {
    index = pet_p4_asset_catalog_find_exact(catalog, canonical);
    if (index < 0) index = pet_p4_asset_catalog_nth_prefix(catalog, "touch.", 0);
    if (index >= 0) {
      behavior->play_video_once = video;
      return select_tracked_asset(behavior, index, now_ms);
    }
  }
  if (!strcmp(canonical, "speaking")) {
    index = pet_p4_asset_catalog_find_exact(catalog, "working.browsing");
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
    canonical = "working";
  }
  if (!strcmp(canonical, "working")) {
    index = rotating_prefix(
      behavior,
      catalog,
      "working",
      now_ms,
      PET_P4_WORKING_VARIANT_FALLBACK_MS,
      asset_revision,
      video
    );
    if (index >= 0) return index;
  }
  if (!strcmp(canonical, "idle")) {
    index = rotating_prefix(
      behavior,
      catalog,
      "idle.",
      now_ms,
      PET_P4_IDLE_VARIANT_FALLBACK_MS,
      asset_revision,
      video
    );
    if (index >= 0) return index;
  }
  return select_tracked_asset(behavior, fallback_idle(catalog), now_ms);
}

int pet_p4_behavior_select(
  pet_p4_behavior_t *behavior, const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle, uint32_t asset_revision, uint64_t now_ms
) {
  return select_behavior(behavior, catalog, lifecycle, asset_revision, now_ms, false, false);
}

int pet_p4_behavior_select_video(
  pet_p4_behavior_t *behavior, const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle, uint32_t asset_revision, uint64_t now_ms, bool allow_touch_tail
) {
  return select_behavior(behavior, catalog, lifecycle, asset_revision, now_ms, true, allow_touch_tail);
}
