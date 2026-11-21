#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* Caller serializes with the existing cue mux. No I/O, clock, heap or DSP.
 * A live task creates a cue only when its observed state becomes done or error.
 * Playback can wait for storage, but storage never creates a task cue.
 * Slots cover the bounded visible queue plus one legacy unscoped source. */
#define PET_COMPLETION_CUE_SLOTS (PET_P4_SESSION_QUEUE_MAX + 1)
#define PET_COMPLETION_AGENT_BYTES 48

typedef enum {
  PET_TASK_CUE_NONE,
  PET_TASK_CUE_DONE,
  PET_TASK_CUE_ERROR,
} pet_task_cue_t;

typedef struct {
  char agent[PET_COMPLETION_AGENT_BYTES];
  char session[PET_P4_SESSION_QUEUE_ID_MAX];
  uint64_t active_revision, terminal_revision, consumed_revision;
  unsigned pending_generation;
  uint8_t outcome;
  bool used, present, active, pending;
} pet_completion_cue_entry_t;
typedef struct {pet_completion_cue_entry_t entries[PET_COMPLETION_CUE_SLOTS];} pet_completion_cue_guard_t;
static inline bool pet_completion_key_valid(const char *agent,const char *session) {
  if(!agent||!session||!agent[0])return false;
  size_t a=0,s=0;while(a<PET_COMPLETION_AGENT_BYTES&&agent[a])a++;
  while(s<PET_P4_SESSION_QUEUE_ID_MAX&&session[s])s++;
  return a<PET_COMPLETION_AGENT_BYTES&&s<PET_P4_SESSION_QUEUE_ID_MAX;
}
static inline pet_completion_cue_entry_t *pet_completion_find(pet_completion_cue_guard_t *g,const char *agent,const char *session) {
  if(!pet_completion_key_valid(agent,session))return NULL;
  for(unsigned i=0;i<PET_COMPLETION_CUE_SLOTS;i++)if(g->entries[i].used&&!strcmp(g->entries[i].agent,agent)&&!strcmp(g->entries[i].session,session))return &g->entries[i];
  return NULL;
}
static inline void pet_completion_snapshot_begin(pet_completion_cue_guard_t *g) {
  for(unsigned i=0;i<PET_COMPLETION_CUE_SLOTS;i++)g->entries[i].present=false;
}
static inline void pet_completion_snapshot_present(pet_completion_cue_guard_t *g,const char *agent,const char *session) {
  pet_completion_cue_entry_t *e=pet_completion_find(g,agent,session);if(e)e->present=true;
}
static inline void pet_completion_observe(pet_completion_cue_guard_t *g,const char *agent,const char *session,uint64_t revision,bool active,pet_task_cue_t outcome) {
  if(!pet_completion_key_valid(agent,session))return;
  pet_completion_cue_entry_t *e=pet_completion_find(g,agent,session);
  /* Never allocate from a terminal snapshot: an evicted historical completion
   * cannot become a new notification merely because it reappeared. */
  if(!e&&active) {
    for(unsigned i=0;i<PET_COMPLETION_CUE_SLOTS;i++)if(!g->entries[i].used||!g->entries[i].present) {
      e=&g->entries[i];memset(e,0,sizeof(*e));e->used=e->present=true;
      memcpy(e->agent,agent,strlen(agent)+1);memcpy(e->session,session,strlen(session)+1);break;
    }
  }
  if(!e)return;
  e->present=true;
  if (active) {
    bool newer = revision > e->terminal_revision && revision >= e->active_revision;
    bool legacy = !revision && !e->active_revision && !e->terminal_revision;
    if (newer || legacy) {
      e->active_revision = revision;
      e->active = true;
      e->outcome = PET_TASK_CUE_NONE;
      e->pending = false;
    }
    return;
  }
  if (revision < e->active_revision || revision < e->terminal_revision) return;
  if (e->active && outcome != PET_TASK_CUE_NONE) {
    e->outcome = outcome;
  } else if (outcome != e->outcome) {
    e->outcome = PET_TASK_CUE_NONE;
    e->pending = false;
  }
  if (outcome != PET_TASK_CUE_NONE) e->terminal_revision = revision;
  e->active = false;
}
static inline void pet_completion_snapshot_end(pet_completion_cue_guard_t *g) {
  for (unsigned i = 0; i < PET_COMPLETION_CUE_SLOTS; i++) {
    pet_completion_cue_entry_t *e = &g->entries[i];
    if (!e->present) {
      e->outcome = PET_TASK_CUE_NONE;
      e->active = e->pending = false;
    }
  }
}
static inline bool pet_completion_matches(const pet_completion_cue_entry_t *e, pet_task_cue_t outcome, uint64_t revision) {
  return e && e->present && outcome != PET_TASK_CUE_NONE && e->outcome == outcome
    && revision >= e->terminal_revision && (!revision || revision > e->consumed_revision);
}
static inline bool pet_completion_due(pet_completion_cue_guard_t *g,const char *agent,const char *session,pet_task_cue_t outcome,uint64_t revision,unsigned current_generation) {
  pet_completion_cue_entry_t *e=pet_completion_find(g,agent,session);
  if (!pet_completion_matches(e, outcome, revision)) return false;
  if (e->pending && e->pending_generation != current_generation) e->pending = false;
  return !e->pending;
}
static inline bool pet_completion_reserve(pet_completion_cue_guard_t *g,const char *agent,const char *session,pet_task_cue_t outcome,uint64_t revision,unsigned generation) {
  pet_completion_cue_entry_t *e=pet_completion_find(g,agent,session);
  if (!pet_completion_matches(e, outcome, revision) || e->pending) return false;
  e->pending = true;
  e->pending_generation = generation;
  return true;
}
static inline bool pet_completion_pending(pet_completion_cue_guard_t *g,unsigned generation) {
  for (unsigned i = 0; i < PET_COMPLETION_CUE_SLOTS; i++) {
    const pet_completion_cue_entry_t *e = &g->entries[i];
    if (e->present && e->pending && e->pending_generation == generation) return true;
  }
  return false;
}
static inline bool pet_completion_cancel(pet_completion_cue_guard_t *g,unsigned generation) {
  for(unsigned i=0;i<PET_COMPLETION_CUE_SLOTS;i++)if(g->entries[i].used&&g->entries[i].pending&&g->entries[i].pending_generation==generation) {g->entries[i].pending=false;return true;}
  return false;
}
static inline bool pet_completion_started(pet_completion_cue_guard_t *g,unsigned generation) {
  for(unsigned i=0;i<PET_COMPLETION_CUE_SLOTS;i++) {
    pet_completion_cue_entry_t *e=&g->entries[i];
    if(e->present && e->pending && e->pending_generation == generation) {
      e->consumed_revision = e->terminal_revision;
      e->outcome = PET_TASK_CUE_NONE;
      e->pending = false;
      return true;
    }
  }
  return false;
}
