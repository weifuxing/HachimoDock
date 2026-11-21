"""Run actual OTA admission, ownership and commit functions with a fake flash."""
import subprocess

from p4_control_integrity_test import ROOT, _c_function


def test_trusted_connection_and_exact_image_own_wireless_ota(tmp_path):
    source = (ROOT / "main/pet_p4_ota.c").read_text()
    functions = (
        "commit_transfer", "wireless_owner_present", "wireless_admitted",
        "wireless_id_hex", "wireless_transfer_matches", "wireless_boot_locked",
        "pet_p4_wireless_ota_boot", "pet_p4_wireless_ota_begin",
        "pet_p4_wireless_ota_chunk", "pet_p4_wireless_ota_commit",
        "pet_p4_wireless_ota_close",
    )
    test = tmp_path / "test.c"
    test.write_text(r'''
#define _POSIX_C_SOURCE 200809L
#include "pet_p4_wireless_ota.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PET_P4_SYSTEM_WIFI 1
#define PET_P4_OTA_RESTART_DELAY_MS 5000
#define ESP_OK 0
#define ESP_OTA_IMG_VALID 2
#define pdTRUE 1
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)

typedef int esp_err_t;
typedef int esp_ota_img_states_t;
typedef struct {uint32_t address; const char *label;} esp_partition_t;
typedef struct {char project_name[32]; char version[32];} esp_app_desc_t;
static const esp_partition_t prior = {0x10000, "ota_0"}, next = {0xa0000, "ota_1"};
static const esp_app_desc_t descriptor = {"wifi6", "0.7.103-p4-r279-wifi6"};
static const esp_partition_t *g_update_partition = &next;
static bool g_active, g_pending_verify, g_sha256_initialized;
static bool g_initialized = true, locked, wifi6 = true;
static char g_transfer_id[64], g_expected_sha256[65];
static uint8_t g_wireless_owner[16], g_wireless_installed_digest[32];
static unsigned g_next_sequence, g_ota_handle, g_sha256;
static unsigned begins, writes, aborts, boot_switches;
static unsigned long long g_expected_size, g_received_size, g_last_activity_ms, g_restart_at_ms;
static int g_ota_mutex = 1;
static struct {
  bool valid, wireless;
  char transfer_id[64], target_partition[17], version[32], project_name[32], sha256[65];
  unsigned long long bytes;
  unsigned next_sequence;
} g_commit_result;
static uint8_t stream_byte = 0x55;
static int xSemaphoreTake(int m, int wait) {assert(m && !locked); (void)wait; locked = true; return 1;}
static void xSemaphoreGive(int m) {assert(m && locked); locked = false;}
static bool pet_p4_is_wifi6_hardware(void) {return wifi6;}
static const char *pet_p4_boot_identity(void) {return "current";}
static const esp_partition_t *esp_ota_get_running_partition(void) {assert(locked); return &prior;}
static int esp_ota_get_state_partition(const esp_partition_t *p, int *state) {
  assert(locked && p == &prior);
  *state = ESP_OTA_IMG_VALID;
  return ESP_OK;
}
static int esp_partition_get_sha256(const esp_partition_t *part, uint8_t *out) {
  assert(locked);
  memset(out, part == &prior ? 0x11 : 0x22, 32);
  return ESP_OK;
}
static void digest_hex(const uint8_t *d, char *out) {
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}
static void clear_transfer(bool abort) {
  assert(locked);
  aborts += abort;
  g_active = false;
  memset(g_wireless_owner, 0, sizeof(g_wireless_owner));
}
static uint64_t esp_timer_get_time(void) {return 1000000;}
static const char *esp_err_to_name(int n) {(void)n; return "native error";}
static const esp_app_desc_t *esp_app_get_description(void) {return &descriptor;}
static int mbedtls_sha256_finish(unsigned *sha, uint8_t *digest) {
  assert(sha == &g_sha256);
  memset(digest, stream_byte, 32);
  return 0;
}
static void mbedtls_sha256_free(unsigned *sha) {assert(sha == &g_sha256);}
static int esp_ota_end(unsigned handle) {assert(locked && handle == 1); return ESP_OK;}
static int esp_ota_get_partition_description(const esp_partition_t *part, esp_app_desc_t *out) {
  assert(locked && part == &next); *out = descriptor; return ESP_OK;
}
static int esp_ota_set_boot_partition(const esp_partition_t *part) {
  assert(locked && part == &next); boot_switches++; return ESP_OK;
}
static const char *begin_transfer(const char *id, const char *sha, unsigned long long size,
                                  bool check_revision, unsigned long long min, unsigned long long max) {
  assert(locked && !g_active && check_revision && min == 300 && max == 399);
  begins++;
  strcpy(g_transfer_id, id);
  strcpy(g_expected_sha256, sha);
  g_expected_size = size; g_received_size = 0; g_next_sequence = 0;
  g_active = true; g_ota_handle = 1; g_sha256_initialized = true;
  return NULL;
}
static const char *write_chunk(unsigned sequence, const uint8_t *bytes, size_t size) {
  assert(locked && g_active && bytes && sequence == g_next_sequence);
  writes++; g_received_size += size; g_next_sequence++;
  return NULL;
}
''' + "\n".join(_c_function(source, name) for name in functions) + r'''
int main(void) {
  pet_p4_wireless_ota_peer_t peer = {.usb_verified = true, .owner = {1}};
  pet_p4_wireless_ota_peer_t other = {.usb_verified = true, .owner = {2}};
  pet_p4_wireless_ota_peer_t untrusted = {.owner = {1}};
  pet_p4_wireless_ota_offer_t offer = {
    .transfer_id = {7}, .image_bytes = 32, .revision_min = 300, .revision_max = 399
  };
  pet_p4_wireless_ota_receipt_t receipt;
  uint8_t data[32] = {0}, wrong_id[16] = {8};

  assert(!pet_p4_wireless_ota_boot(&peer, &offer.prior));
  memset(offer.image_sha256, 0x55, 32);
  memset(offer.installed_digest, 0x22, 32);
  assert(pet_p4_wireless_ota_begin(&untrusted, &offer));
  wifi6 = false;
  assert(pet_p4_wireless_ota_begin(&peer, &offer));
  wifi6 = true;
  offer.prior.digest[0] ^= 1;
  assert(pet_p4_wireless_ota_begin(&peer, &offer));
  offer.prior.digest[0] ^= 1;
  strcpy(offer.prior.boot_id, "old");
  assert(pet_p4_wireless_ota_begin(&peer, &offer));
  assert(!pet_p4_wireless_ota_boot(&peer, &offer.prior));
  g_pending_verify = true;
  assert(pet_p4_wireless_ota_begin(&peer, &offer) && begins == 0);
  g_pending_verify = false;

  g_active = true;  // Active USB transfer has no wireless owner.
  assert(pet_p4_wireless_ota_begin(&peer, &offer));
  assert(pet_p4_wireless_ota_chunk(&peer, offer.transfer_id, 0, data, sizeof(data)));
  pet_p4_wireless_ota_close(peer.owner);
  assert(g_active && aborts == 0);
  g_active = false;

  assert(!pet_p4_wireless_ota_begin(&peer, &offer));
  assert(pet_p4_wireless_ota_chunk(&other, offer.transfer_id, 0, data, sizeof(data)));
  assert(pet_p4_wireless_ota_chunk(&peer, wrong_id, 0, data, sizeof(data)));
  assert(pet_p4_wireless_ota_chunk(&peer, offer.transfer_id, 1, data, sizeof(data)));
  assert(pet_p4_wireless_ota_commit(&other, offer.transfer_id, &receipt));
  pet_p4_wireless_ota_close(other.owner);
  assert(g_active && writes == 0 && boot_switches == 0 && aborts == 0);
  pet_p4_wireless_ota_close(peer.owner);
  assert(!g_active && aborts == 1);

  assert(!pet_p4_wireless_ota_begin(&peer, &offer));
  assert(pet_p4_wireless_ota_commit(&peer, offer.transfer_id, &receipt));
  assert(!pet_p4_wireless_ota_chunk(&peer, offer.transfer_id, 0, data, sizeof(data)));
  assert(pet_p4_wireless_ota_chunk(&peer, offer.transfer_id, 0, data, sizeof(data)));
  stream_byte = 0x99;
  assert(pet_p4_wireless_ota_commit(&peer, offer.transfer_id, &receipt));
  assert(!g_active && boot_switches == 0 && aborts == 2);

  offer.installed_digest[0] ^= 1;
  stream_byte = 0x55;
  assert(!pet_p4_wireless_ota_begin(&peer, &offer));
  assert(!pet_p4_wireless_ota_chunk(&peer, offer.transfer_id, 0, data, sizeof(data)));
  assert(pet_p4_wireless_ota_commit(&peer, offer.transfer_id, &receipt));
  assert(!receipt.address && !g_active && boot_switches == 0);
  offer.installed_digest[0] ^= 1;

  assert(!pet_p4_wireless_ota_begin(&peer, &offer));
  assert(!pet_p4_wireless_ota_chunk(&peer, offer.transfer_id, 0, data, sizeof(data)));
  assert(!pet_p4_wireless_ota_commit(&peer, offer.transfer_id, &receipt));
  assert(receipt.address == next.address && receipt.installed_digest[0] == 0x22);
  assert(g_commit_result.wireless && g_restart_at_ms && boot_switches == 1);
  pet_p4_wireless_ota_close(peer.owner);
  assert(pet_p4_wireless_ota_commit(&peer, offer.transfer_id, &receipt));
  assert(boot_switches == 1 && aborts == 2 && !locked);
}
''')
    binary = tmp_path / "test"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
         str(test), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
