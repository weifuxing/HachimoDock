/* Mode 12 extends the paired handshake with ordered OTA records on one socket.
 * The initial request and reply already use nonce domains 0 and 1. Each
 * subsequent record binds its length and uses domains 2 (rx) and 3 (tx). */
#include "esp_chip_info.h"
#include "pet_p4_wireless_ota.h"

#define LAN_OTA_MODE 12
#define LAN_OTA_FRAME_MAX (1 + 16 + 4 + PET_P4_WIRELESS_OTA_CHUNK_BYTES)
#define LAN_OTA_FRAME_WAIT_US 5000000
#define LAN_OTA_SESSION_WAIT_US 420000000

static uint32_t ota_wire_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
         (uint32_t)bytes[2] << 8 | bytes[3];
}

static void ota_wire_put_u32(uint8_t *bytes, uint32_t value) {
  bytes[0] = value >> 24;
  bytes[1] = value >> 16;
  bytes[2] = value >> 8;
  bytes[3] = value;
}

static bool ota_wire_trusted(uint32_t request_generation) {
  if (!atomic_load(&control_trusted) || atomic_load(&forget_pending) ||
      !pet_device_wifi_online()) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool current = paired && generation == request_generation;
  xSemaphoreGive(lock);
  return current;
}

static size_t ota_wire_request(const pet_p4_wireless_ota_peer_t *peer, uint8_t *plain, size_t size) {
  const char *error = "invalid wireless OTA record";
  size_t reply_size = 1;
  if (plain[0] == 0 && size == 1) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    plain[1] = chip.revision >> 8;
    plain[2] = chip.revision;
    if (esp_read_mac(plain + 3, ESP_MAC_BASE) == ESP_OK) {
      error = NULL;
      reply_size = 9;
    }
  } else if (plain[0] == 1 && size == 1) {
    pet_p4_wireless_ota_boot_t boot;
    error = pet_p4_wireless_ota_boot(peer, &boot);
    if (!error) {
      if (strlen(boot.boot_id) != 16) return 0;
      plain[1] = boot.valid;
      memcpy(plain + 2, boot.boot_id, 16);
      ota_wire_put_u32(plain + 18, boot.address);
      memcpy(plain + 22, boot.digest, sizeof(boot.digest));
      reply_size = 54;
    }
  } else if (plain[0] == 2 && size == 141) {
    pet_p4_wireless_ota_offer_t offer = {.prior.valid = true};
    memcpy(offer.prior.boot_id, plain + 1, 16);
    offer.prior.address = ota_wire_u32(plain + 17);
    memcpy(offer.prior.digest, plain + 21, 32);
    memcpy(offer.transfer_id, plain + 53, 16);
    memcpy(offer.image_sha256, plain + 69, 32);
    memcpy(offer.installed_digest, plain + 101, 32);
    offer.image_bytes = ota_wire_u32(plain + 133);
    offer.revision_min = (uint16_t)plain[137] << 8 | plain[138];
    offer.revision_max = (uint16_t)plain[139] << 8 | plain[140];
    error = pet_p4_wireless_ota_begin(peer, &offer);
  } else if (plain[0] == 3 && size > 21) {
    error = pet_p4_wireless_ota_chunk(peer, plain + 1, ota_wire_u32(plain + 17), plain + 21, size - 21);
  } else if (plain[0] == 4 && size == 17) {
    pet_p4_wireless_ota_receipt_t receipt;
    error = pet_p4_wireless_ota_commit(peer, plain + 1, &receipt);
    if (!error) {
      ota_wire_put_u32(plain + 1, receipt.address);
      memcpy(plain + 5, receipt.installed_digest, sizeof(receipt.installed_digest));
      reply_size = 37;
    }
  }
  plain[0] = error ? 0 : 1;
  if (error) {
    reply_size = strnlen(error, 120) + 1;
    memcpy(plain + 1, error, reply_size - 1);
  }
  return reply_size;
}

static void ota_wire_session(int fd, mbedtls_gcm_context *gcm, uint8_t *cipher,
                             uint8_t *plain, uint32_t request_generation) {
  pet_p4_wireless_ota_peer_t peer = {.usb_verified = true};
  esp_fill_random(peer.owner, sizeof(peer.owner));
  const int enabled = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  const int64_t end = esp_timer_get_time() + LAN_OTA_SESSION_WAIT_US;
  for (uint32_t sequence = 0; sequence < 32768 && ota_wire_trusted(request_generation); ++sequence) {
    const int64_t next = esp_timer_get_time() + LAN_OTA_FRAME_WAIT_US;
    const int64_t deadline = next < end ? next : end;
    uint8_t length[2], nonce[12] = {2};
    ota_wire_put_u32(nonce + 8, sequence);
    if (!socket_io(fd, length, sizeof(length), false, deadline)) break;
    const size_t size = (size_t)length[0] << 8 | length[1];
    if (!size || size > LAN_OTA_FRAME_MAX ||
        !socket_io(fd, cipher, size + LAN_TAG_SIZE, false, deadline) ||
        mbedtls_gcm_auth_decrypt(gcm, size, nonce, sizeof(nonce), length, sizeof(length),
          cipher + size, LAN_TAG_SIZE, cipher, plain) ||
        !ota_wire_trusted(request_generation)) break;
    const bool terminal = plain[0] == 4;
    const size_t reply = ota_wire_request(&peer, plain, size);
    const bool accepted = reply && plain[0] == 1;
    nonce[0] = 3;
    length[0] = reply >> 8;
    length[1] = reply;
    const int64_t send_deadline = esp_timer_get_time() + LAN_IO_US;
    if (!reply || mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, reply, nonce, sizeof(nonce),
          length, sizeof(length), plain, cipher + 2, LAN_TAG_SIZE, cipher + 2 + reply)) break;
    memcpy(cipher, length, sizeof(length));
    if (!socket_io(fd, cipher, 2 + reply + LAN_TAG_SIZE, true, send_deadline) ||
        !accepted || terminal) break;
  }
  mbedtls_platform_zeroize(plain, LAN_OTA_FRAME_MAX);
  pet_p4_wireless_ota_close(peer.owner);
}
