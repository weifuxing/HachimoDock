"""Run the on-device credential record code against software mbedTLS and fake NVS."""
import os
from pathlib import Path
import subprocess

import pytest

FIRMWARE = Path(__file__).resolve().parents[1]


def test_private_feed_records_are_sealed_bound_and_reopenable(tmp_path):
    sdk = os.environ.get("PETUI_TEST_MBEDTLS_SOURCE")
    if not sdk:
        pytest.skip("set PETUI_TEST_MBEDTLS_SOURCE to the ESP-IDF mbedTLS source tree")
    mbed = Path(sdk)
    (tmp_path / "freertos").mkdir()
    stubs = {
        "bootloader_random.h": "void bootloader_random_enable(void); void bootloader_random_disable(void);",
        "esp_random.h": "#include <stddef.h>\nvoid esp_fill_random(void *, size_t);",
        "esp_efuse.h": """
#pragma once
#include <stdbool.h>
typedef int esp_efuse_block_t;
enum { EFUSE_BLK_KEY0 = 4, ESP_EFUSE_KEY_PURPOSE_HMAC_UP = 8 };
bool esp_efuse_find_purpose(int, esp_efuse_block_t *);
bool esp_efuse_get_key_dis_read(esp_efuse_block_t);
bool esp_efuse_get_key_dis_write(esp_efuse_block_t);
bool esp_efuse_get_keypurpose_dis_write(esp_efuse_block_t);
""",
        "esp_hmac.h": """
#include <stddef.h>
#include <stdint.h>
typedef int hmac_key_id_t;
int esp_hmac_calculate(hmac_key_id_t, const void *, size_t, uint8_t *);
""",
        "nvs.h": """
#include <stddef.h>
enum { ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READONLY = 0, NVS_READWRITE = 1 };
typedef int nvs_handle_t;
typedef int esp_err_t;
int nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_commit(nvs_handle_t);
int nvs_erase_key(nvs_handle_t, const char *);
""",
        "freertos/FreeRTOS.h": "#define pdPASS 1",
        "freertos/task.h": """
int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *);
void vTaskDelete(void *);
""",
        "petui_mbedtls_config.h": """
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
""",
    }
    for path, content in stubs.items():
        (tmp_path / path).write_text(content)

    harness = tmp_path / "vault.c"
    harness.write_text(r'''
#include <assert.h>
#include "pet_p4_private_feed.c"

static unsigned test_seed, size[8];
static bool root_provisioned, entropy_on;
static uint8_t blobs[8][512];

bool esp_efuse_find_purpose(int purpose, esp_efuse_block_t *block) {
  assert(purpose == ESP_EFUSE_KEY_PURPOSE_HMAC_UP);
  *block = EFUSE_BLK_KEY0;
  return root_provisioned;
}
bool esp_efuse_get_key_dis_read(esp_efuse_block_t b) { return b == EFUSE_BLK_KEY0; }
bool esp_efuse_get_key_dis_write(esp_efuse_block_t b) { return b == EFUSE_BLK_KEY0; }
bool esp_efuse_get_keypurpose_dis_write(esp_efuse_block_t b) { return b == EFUSE_BLK_KEY0; }
int esp_hmac_calculate(hmac_key_id_t id, const void *domain, size_t n, uint8_t *key) {
  assert(!id && root_provisioned);
  return mbedtls_sha256(domain, n, key, 0);
}
void bootloader_random_enable(void) { assert(!entropy_on); entropy_on = true; }
void bootloader_random_disable(void) { assert(entropy_on); entropy_on = false; }
void esp_fill_random(void *data, size_t n) { assert(entropy_on); memset(data, ++test_seed, n); }
int nvs_open(const char *ns, int mode, nvs_handle_t *h) {
  assert(!strcmp(ns, "pet_credentials"));
  *h = mode;
  return 0;
}
void nvs_close(nvs_handle_t h) { (void)h; }
int nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *n) {
  (void)h;
  unsigned i = key[4] - '0';
  assert(i < 8 && *n >= size[i]);
  if (!size[i]) return ESP_ERR_NVS_NOT_FOUND;
  memcpy(data, blobs[i], size[i]);
  *n = size[i];
  return 0;
}
int nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t n) {
  unsigned i = key[4] - '0';
  assert(h == NVS_READWRITE && i < 8 && n <= sizeof(blobs[i]));
  memcpy(blobs[i], data, n);
  size[i] = n;
  return 0;
}
int nvs_erase_key(nvs_handle_t h, const char *key) {
  assert(h == NVS_READWRITE && key[4] >= '0' && key[4] <= '7');
  size[key[4] - '0'] = 0;
  return 0;
}
int nvs_commit(nvs_handle_t h) { assert(h == NVS_READWRITE); return 0; }
int xTaskCreate(void (*f)(void *), const char *name, int stack, void *arg, int prio, void *out) {
  (void)name; (void)stack; (void)prio; (void)out; f(arg); return pdPASS;
}
void vTaskDelete(void *task) { (void)task; }

int main(void) {
  const char *token = "fixture-bearer-secret";
  pet_private_feed_t feed, other;
  pet_wifi_endpoint_t endpoint;
  pet_private_secret_t typed = {0};
  cJSON *connection = cJSON_Parse("{\"type\":\"bearer\"}");
  assert(pet_private_feed_parse(&feed, &endpoint, "local", "https://data.example.org/data", connection));
  assert(pet_private_feed_parse(&other, &endpoint, "local", "https://data.example.org/other", connection));
  pet_private_feed_t invalid;
  assert(!pet_private_feed_parse(&invalid, &endpoint, "local", "https://data.example.org/data?k=secret", connection));
  cJSON_Delete(connection);
  strcpy(typed.token, token);
  pet_private_storage_init();
  assert(!pet_private_secret_schedule(&feed, &typed, false));
  root_provisioned = true;
  pet_private_storage_init();
  assert(pet_private_secret_schedule(&feed, &typed, false));
  assert(!entropy_on && pet_private_local_result() == PET_PRIVATE_SET);
  for (unsigned i = 0; i + strlen(token) <= size[0]; i++)
    assert(memcmp(blobs[0] + i, token, strlen(token)));

  memset(&typed, 0, sizeof(typed));
  assert(pet_private_secret_read(&other, &typed) == PET_PRIVATE_ABSENT);
  assert(pet_private_secret_read(&feed, &typed) == PET_PRIVATE_SET && !strcmp(typed.token, token));
  uint8_t iv[12];
  memcpy(iv, blobs[0] + 32, sizeof(iv));
  assert(pet_private_secret_schedule(&feed, &typed, false));
  assert(memcmp(iv, blobs[0] + 32, sizeof(iv)));
  blobs[0][size[0] - 1] ^= 1;
  assert(pet_private_secret_read(&feed, &typed) == PET_PRIVATE_STORAGE_ERROR && !typed.token[0]);
  assert(pet_private_secret_schedule(&feed, &typed, true));
  assert(pet_private_secret_read(&feed, &typed) == PET_PRIVATE_ABSENT);
}
''')
    cjson = FIRMWARE / "tests/vendor/cjson"
    crypto = ("aes", "cipher", "cipher_wrap", "constant_time", "gcm", "sha256", "platform_util")
    binary = tmp_path / "vault"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-DPET_P4_SYSTEM_WIFI=1",
        '-DMBEDTLS_CONFIG_FILE="petui_mbedtls_config.h"',
        "-I", str(tmp_path), "-I", str(FIRMWARE / "main"), "-I", str(cjson),
        "-I", str(mbed / "include"), str(harness), str(cjson / "cJSON.c"),
        *(str(mbed / "library" / (name + ".c")) for name in crypto),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
