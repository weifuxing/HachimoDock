#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PET_P4_LAN_AUTO_CONTEXT "Hachimodock-auto-v1"
#define PET_P4_LAN_DISCOVERY_PREFIX "HACHI-DISCOVER-1 "

static inline const char *pet_p4_lan_discovery_nonce(const char *data, size_t size) {
  const size_t prefix = sizeof(PET_P4_LAN_DISCOVERY_PREFIX) - 1;
  if (!data || size != prefix + 32 || memcmp(data, PET_P4_LAN_DISCOVERY_PREFIX, prefix)) return NULL;
  for (size_t i = prefix; i < size; ++i)
    if (!((data[i] >= '0' && data[i] <= '9') || (data[i] >= 'a' && data[i] <= 'f'))) return NULL;
  return data + prefix;
}

/* Never truncate the SRP context to the legacy four-character PIN. */
static inline size_t pet_p4_lan_identity_password(char *out, size_t capacity, const char *context) {
  if (!out || !capacity || !context) return 0;
  int n = snprintf(out, capacity, "Hachimodock:%s", context);
  if (n <= 0 || (size_t)n >= capacity) { out[0] = 0; return 0; }
  return (size_t)n;
}

/* Discovery and commit share this policy; a saved binding is never overwritten. */
static inline bool pet_p4_lan_can_pair(bool online, bool state_known, bool paired, bool forgetting) {
  return online && state_known && !paired && !forgetting;
}
