#pragma once
#include "pet_p4_conversation.h"
#include <stdint.h>
#include <string.h>

/* RTC kind 7, rtcKey bit 2. Fixed fields match the USB conversation limits.
 * All mailbox calls run under its short critical section; apply also owns the
 * state mutex. Only bounded memory operations are permitted in this helper.
 * The authenticated socket/boot owns the mailbox, never an arbitrary JSON id. */
enum { PET_LAN_HUD_KIND = 7, PET_LAN_HUD_BYTES = 644 };
typedef struct {
  uint8_t body[PET_LAN_HUD_BYTES];
  uint8_t final[PET_LAN_HUD_BYTES];
  char displayed[64];
  bool live, bound, dirty, final_pending, terminal;
} pet_lan_hud_t;

static inline void pet_lan_hud_begin(pet_lan_hud_t *h) {
  h->live = true;
  h->bound = h->dirty = h->final_pending = h->terminal = false;
}

static inline bool pet_lan_hud_offer(pet_lan_hud_t *h, const uint8_t *body) {
  if (!h->live || body[0] > 5 || body[1] > 3 || body[2] || body[3]
      || !body[4] || !memchr(body + 4, 0, 64) || !memchr(body + 68, 0, 64)
      || !memchr(body + 132, 0, 512)
      || (!h->bound && body[0] != 0)
      || (h->bound && strcmp((const char *)h->body + 4, (const char *)body + 4))) return false;
  /* Passive preparation heartbeats must not reset caption timing/history. */
  if (!h->bound || memcmp(h->body, body, PET_LAN_HUD_BYTES)) {
    /* Never block the audio socket on a stalled renderer. Keep the newest
     * final caption separately when later progress replaces it. At overload
     * this retains at most two finals (saved + latest), not an unbounded FIFO. */
    if (h->dirty && (h->body[1] & 2)) {
      memcpy(h->final, h->body, PET_LAN_HUD_BYTES);
      h->final_pending = true;
    }
    memcpy(h->body, body, PET_LAN_HUD_BYTES);
    h->dirty = true;
  }
  h->bound = true;
  return true;
}

static inline void pet_lan_hud_end(pet_lan_hud_t *h) {
  h->live = h->dirty = h->final_pending = h->terminal = false;
}

/* Only an authenticated, still-owned Close may preserve its terminal HUD.
 * Transport failure/revocation uses end instead, discarding pending updates. */
static inline void pet_lan_hud_close(pet_lan_hud_t *h) {
  if (h->bound && h->body[0] >= 4) {
    h->live = false;
    h->terminal = true;
  } else pet_lan_hud_end(h);
}

static inline bool pet_lan_hud_update(pet_p4_runtime_state_t *s, const uint8_t *body, uint64_t now) {
  static const char *const phases[] = {
    "preparing", "listening", "thinking", "speaking", "ended", "error"
  };
  return pet_p4_conversation_update(s, (const char *)body + 4, phases[body[0]],
      (const char *)body + 68, (const char *)body + 132, body[1] & 1, body[1] & 2, now);
}

static inline void pet_lan_hud_apply(pet_lan_hud_t *h, pet_p4_runtime_state_t *s,
                                    uint64_t now, bool usb) {
  if (usb) pet_lan_hud_end(h);
  if ((!h->live && !h->terminal) || !h->bound) {
    if (h->displayed[0] && !strcmp(s->conversation_id, h->displayed))
      pet_p4_conversation_clear(s);
    h->displayed[0] = 0;
  } else if (h->dirty) {
    const char *id = (const char *)h->body + 4;
    /* Retire A before processing B's terminal packet, which cannot itself
     * clear an unrelated id. Never clear a replacement USB conversation. */
    if (h->displayed[0] && strcmp(h->displayed, id)) {
      if (!strcmp(s->conversation_id, h->displayed)) pet_p4_conversation_clear(s);
      h->displayed[0] = 0;
    }
    /* End before first consumption must never briefly reopen hidden UI. */
    if (h->body[0] != 4 && (!pet_p4_conversation_active(s) || strcmp(s->conversation_id, id)))
      pet_p4_conversation_update(s, id, "preparing", (const char *)h->body + 68, "", false, false, now);
    if (h->body[0] != 4 && h->final_pending) pet_lan_hud_update(s, h->final, now);
    if (pet_lan_hud_update(s, h->body, now)) {
      memcpy(h->displayed, id, sizeof(h->displayed));
      if (h->body[0] == 4) pet_lan_hud_end(h);
    }
    h->dirty = h->final_pending = false;
  }
  /* Shared conversation expiry uses generic host activity. Do not refresh
   * usb_client_last_seen_ms: this authenticated owner is still a LAN client. */
  if ((h->live || h->terminal) && h->displayed[0] && !strcmp(s->conversation_id, h->displayed))
    s->host_last_seen_ms = now;
}
