#pragma once
/* Volatile, one-desktop realtime key mailbox. Caller serializes all access. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#define PET_LAN_PRESS_TTL 3000u
#define PET_LAN_PRESS_LEASE 6000u
#define PET_LAN_PRESS_CAPACITY 4u
typedef struct { uint32_t seq; uint64_t until; uint8_t context; } pet_lan_press_event_t;
typedef struct {
  uint8_t owner[16];
  uint64_t lease, since;
  uint32_t sequence, acknowledged;
  unsigned count;
  pet_lan_press_event_t events[PET_LAN_PRESS_CAPACITY];
} pet_lan_press_t;

static inline void pet_lan_press_clear(pet_lan_press_t *q) {
  q->lease = q->since = 0; q->count = 0; q->acknowledged = 0;
  memset(q->owner, 0, sizeof(q->owner));
}
static inline void pet_lan_press_pop(pet_lan_press_t *q) {
  if (q->count) { --q->count; memmove(q->events, q->events + 1, q->count * sizeof(q->events[0])); }
}
static inline void pet_lan_press_refresh(pet_lan_press_t *q, uint64_t now, bool allowed) {
  if (!allowed || now >= q->lease) pet_lan_press_clear(q);
  while (q->count && now >= q->events[0].until) pet_lan_press_pop(q);
}
static inline bool pet_lan_press_push(pet_lan_press_t *q, uint64_t now, uint64_t pressed, uint8_t context) {
  pet_lan_press_refresh(q, now, true);
  if (!q->lease || pressed > now || now - pressed >= PET_LAN_PRESS_TTL || pressed < q->since || context > 2 || q->count == PET_LAN_PRESS_CAPACITY || q->sequence == UINT32_MAX) return false;
  q->events[q->count++] = (pet_lan_press_event_t){++q->sequence, pressed + PET_LAN_PRESS_TTL, context};
  return true;
}
/* Poll (ack=0) only peeks. Only this owner may ACK the exact live head. */
static inline bool pet_lan_press_poll(pet_lan_press_t *q, uint64_t now,
    const uint8_t owner[16], uint32_t ack) {
  uint8_t nonzero = 0;
  for (unsigned i = 0; i < 16; ++i) nonzero |= owner[i];
  pet_lan_press_refresh(q, now, true);
  if (!nonzero || (!q->lease && ack)) return false;
  if (!q->lease) { memcpy(q->owner, owner, 16); q->since = now; }
  if (memcmp(q->owner, owner, 16)) return false;
  if (ack && ack != q->acknowledged) {
    if (!q->count || q->events[0].seq != ack) return false;
    q->acknowledged = ack; pet_lan_press_pop(q);
  }
  q->lease = now + PET_LAN_PRESS_LEASE;
  return true;
}
