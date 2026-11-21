#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_P4_WIRELESS_OTA_OWNER_BYTES 16
#define PET_P4_WIRELESS_OTA_DIGEST_BYTES 32
#define PET_P4_WIRELESS_OTA_BOOT_ID_MAX 32
#define PET_P4_WIRELESS_OTA_CHUNK_BYTES 4096

/* Construct from authenticated LAN connection state, never from packet fields.
 * Each connection needs a fresh nonzero owner. Recheck USB-backed trust on
 * every call. The authenticated transport must serialize frames per owner. */
typedef struct {
  bool usb_verified;
  uint8_t owner[PET_P4_WIRELESS_OTA_OWNER_BYTES];
} pet_p4_wireless_ota_peer_t;

typedef struct {
  char boot_id[PET_P4_WIRELESS_OTA_BOOT_ID_MAX];
  uint8_t digest[PET_P4_WIRELESS_OTA_DIGEST_BYTES];
  uint32_t address;
  bool valid;
} pet_p4_wireless_ota_boot_t;

typedef struct {
  pet_p4_wireless_ota_boot_t prior;
  uint8_t transfer_id[PET_P4_WIRELESS_OTA_OWNER_BYTES];
  uint8_t image_sha256[PET_P4_WIRELESS_OTA_DIGEST_BYTES];
  uint8_t installed_digest[PET_P4_WIRELESS_OTA_DIGEST_BYTES];
  uint32_t image_bytes;
  uint16_t revision_min;
  uint16_t revision_max;
} pet_p4_wireless_ota_offer_t;

typedef struct {
  uint8_t installed_digest[PET_P4_WIRELESS_OTA_DIGEST_BYTES];
  uint32_t address;
} pet_p4_wireless_ota_receipt_t;

/* WiFi 6 builds only. Return NULL on success, or a static error string.
 * The wire handler should serialize only after these calls release the OTA
 * mutex. Never replay a mutation after losing its result. */
const char *pet_p4_wireless_ota_boot(const pet_p4_wireless_ota_peer_t *peer,
                                    pet_p4_wireless_ota_boot_t *out);
const char *pet_p4_wireless_ota_begin(const pet_p4_wireless_ota_peer_t *peer,
                                     const pet_p4_wireless_ota_offer_t *offer);
const char *pet_p4_wireless_ota_chunk(const pet_p4_wireless_ota_peer_t *peer,
                                    const uint8_t transfer_id[PET_P4_WIRELESS_OTA_OWNER_BYTES],
                                    uint32_t sequence, const uint8_t *data, size_t size);
const char *pet_p4_wireless_ota_commit(const pet_p4_wireless_ota_peer_t *peer,
                                     const uint8_t transfer_id[PET_P4_WIRELESS_OTA_OWNER_BYTES],
                                     pet_p4_wireless_ota_receipt_t *out);
/* Call on connection close or trust revocation, with the local connection's
 * owner even if trust was cleared. A committed image cannot be cancelled. */
void pet_p4_wireless_ota_close(const uint8_t owner[PET_P4_WIRELESS_OTA_OWNER_BYTES]);
