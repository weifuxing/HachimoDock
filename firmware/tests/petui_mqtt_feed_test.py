"""Host-check the production length decoder and exact-topic packet delivery."""
from pathlib import Path
import os
import subprocess

FIRMWARE = Path(__file__).resolve().parents[1]


def run_fixture(tmp_path, source):
    runner = tmp_path / "mqtt_feed.c"
    runner.write_text(source)
    binary = tmp_path / "mqtt_feed"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                    "-I", str(FIRMWARE / "main"), str(runner), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_mqtt_feed_accepts_only_bounded_readonly_packets(tmp_path):
    text = (FIRMWARE / "main/pet_p4_mqtt_feed.c").read_text()
    start = text.index("static bool accept_publish")
    end = text.index("bool pet_mqtt_feed_run")
    run_fixture(tmp_path, r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_mqtt_wire.h"
typedef struct { char topic[128]; } pet_private_feed_t;
typedef void (*pet_mqtt_feed_value_fn)(const char *, size_t, void *);
#define PET_WIFI_DATA_BODY_MAX 16384
''' + text[start:end] + r'''
static void accepted(const char *source, size_t n, void *ctx) {
  assert(n == 2 && !memcmp(source, "{}", n));
  ++*(unsigned *)ctx;
}
int main(void) {
  for (size_t i = 0; i <= PET_MQTT_BODY_LIMIT; ++i) {
    uint8_t encoded[3];
    size_t got = 0;
    size_t digits = pet_mqtt_length_encode(encoded, i);
    assert(digits >= 1 && digits <= 3);
    for (size_t j = 0; j < digits; ++j) {
      assert(pet_mqtt_length_digit(&got, j, encoded[j]));
      assert(!!(encoded[j] & 128) == (j + 1 < digits));
    }
    assert(got == i);
  }
  size_t bad = 0;
  assert(!pet_mqtt_length_digit(&bad, 1, 0));
  assert(!pet_mqtt_length_digit(&bad, 2, 128));
  pet_private_feed_t feed = {.topic = "a/x"};
  unsigned applied = 0;
  const char packet[] = {0, 3, 'a', '/', 'x', '{', '}'};
  assert(accept_publish(&feed, 0x31, packet, sizeof(packet), accepted, &applied));
  assert(!accept_publish(&feed, 0x32, packet, sizeof(packet), accepted, &applied));
  assert(!accept_publish(&feed, 0x30, packet, 3, accepted, &applied));
  feed.topic[2] = 'y';
  assert(!accept_publish(&feed, 0x30, packet, sizeof(packet), accepted, &applied));
  assert(applied == 1);
}
''')


def test_mqtt_session_subscribes_without_publishing_and_handles_fragmentation(tmp_path):
    text = (FIRMWARE / "main/pet_p4_mqtt_feed.c").read_text()
    production = text[text.index("enum { IO_MS"):text.rindex("#endif")]
    run_fixture(tmp_path, r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "pet_p4_mqtt_wire.h"
typedef struct { char host[128]; } pet_wifi_endpoint_t;
#define PET_PUBLIC_MQTT 3
#define pet_live_tls_connect_public_port pet_live_tls_connect_port
typedef struct { unsigned kind; uint16_t port; char topic[128]; } pet_private_feed_t;
typedef struct { char user[65], token[257]; } pet_private_secret_t;
typedef bool (*pet_mqtt_feed_current_fn)(void *);
typedef void (*pet_mqtt_feed_value_fn)(const char *, size_t, void *);
typedef struct { bool non_block; int timeout_ms; void (*crt_bundle_attach)(void); } esp_tls_cfg_t;
typedef int esp_tls_t;
#define PET_PRIVATE_TOPIC_MAX 127
#define PET_WIFI_DATA_BODY_MAX 16384
#define ESP_TLS_ERR_SSL_WANT_READ -2
#define ESP_TLS_ERR_SSL_WANT_WRITE -3
#define pdMS_TO_TICKS(ms) (ms)
static esp_tls_t session;
static uint8_t written[128];
static size_t written_len, read_at;
static bool closed;
static int64_t timestamp;
static void esp_crt_bundle_attach(void) {}
static unsigned esp_random(void) { return 1; }
static int64_t esp_timer_get_time(void) { return timestamp * 1000; }
static void vTaskDelay(int ms) { timestamp += ms; }
static void mbedtls_platform_zeroize(void *out, size_t size) { memset(out, 0, size); }
static esp_tls_t *pet_live_tls_connect_port(const char *host, uint16_t port,
    const esp_tls_cfg_t *cfg, int64_t deadline, pet_mqtt_feed_current_fn current, void *ctx) {
  assert(!strcmp(host, "lab.example.org") && port == 8883);
  assert(cfg->non_block && cfg->crt_bundle_attach && current(ctx));
  return &session;
}
static void esp_tls_conn_destroy(esp_tls_t *tls) { assert(tls == &session); closed = true; }
static ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t n) {
  assert(tls == &session && n && written_len < sizeof(written));
  written[written_len++] = *(const uint8_t *)data;
  return 1;
}
static ssize_t esp_tls_conn_read(esp_tls_t *tls, void *data, size_t n) {
  static const uint8_t initial[] = {
    0x20, 2, 0, 0,                           /* CONNACK */
    0x31, 5, 0, 1, 't', '{', '}',             /* Retained PUBLISH */
    0x90, 3, 0, 1, 0,                        /* SUBACK */
  };
  static const uint8_t after_ping[] = {
    0xd0, 0,                                 /* PINGRESP */
    0x30, 5, 0, 1, 't', '[', ']',             /* Live PUBLISH */
    0x30, 5, 0, 1, 'x', '[', ']',             /* Unrelated topic: must reject */
  };
  assert(tls == &session && n);
  if (read_at < sizeof(initial)) {
    *(uint8_t *)data = initial[read_at++];
    return 1;
  }
  if (written_len < 2 || written[written_len - 2] != 0xc0)
    return ESP_TLS_ERR_SSL_WANT_READ;
  size_t index = read_at++ - sizeof(initial);
  if (index >= sizeof(after_ping)) return 0;
  *(uint8_t *)data = after_ping[index];
  return 1;
}
''' + production + r'''
static bool current(void *ctx) { return true; }
static void accepted(const char *source, size_t n, void *ctx) {
  unsigned *count = ctx;
  assert(*count < 2 && n == 2 && !memcmp(source, *count ? "[]" : "{}", n));
  ++*count;
}
int main(void) {
  const pet_wifi_endpoint_t endpoint = {.host = "lab.example.org"};
  const pet_private_feed_t feed = {.port = 8883, .topic = "t"};
  const pet_private_secret_t secret = {.user = "u", .token = "p"};
  char workspace[PET_MQTT_BODY_LIMIT] = {0};
  unsigned accepted_count = 0;
  assert(pet_mqtt_feed_run(&endpoint, &feed, &secret, workspace, current, accepted, &accepted_count));
  assert(closed && accepted_count == 2);
  assert(written[0] == 0x10 && written[9] == 0xc2);
  const size_t subscribe_at = 2 + written[1];
  const uint8_t subscribe_and_ping[] = {0x82, 6, 0, 1, 0, 1, 't', 0, 0xc0, 0};
  assert(written_len == subscribe_at + sizeof(subscribe_and_ping));
  assert(!memcmp(written + subscribe_at, subscribe_and_ping, sizeof(subscribe_and_ping)));
  for (size_t i = 0; i < sizeof(workspace); i++) assert(!workspace[i]);
  written_len = read_at = timestamp = accepted_count = 0;
  closed = false;
  pet_private_feed_t public_feed = feed;
  public_feed.kind = PET_PUBLIC_MQTT;
  const pet_private_secret_t anonymous = {0};
  assert(pet_mqtt_feed_run(&endpoint, &public_feed, &anonymous, workspace, current, accepted, &accepted_count));
  assert(closed && accepted_count == 2 && written[9] == 2);
  assert(written[1] + 6u == subscribe_at - 2u);
}
''')
