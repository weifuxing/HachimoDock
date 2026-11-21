#include "pet_p4_mqtt_feed.h"

#if PET_P4_SYSTEM_WIFI
#include <stdio.h>
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "mbedtls/platform_util.h"
#include "pet_p4_live_tls.h"
#include "pet_p4_mqtt_wire.h"
#include "pet_p4_wifi_data_sources.h"

enum { IO_MS = 5000, PING_MS = 15000 };

static int64_t clock_ms(void) { return esp_timer_get_time() / 1000; }

typedef struct {
  esp_tls_t *tls;
  pet_mqtt_feed_current_fn current;
  void *context;
} mqtt_feed_t;

/* Zero means an idle deadline. EOF, cancellation and TLS errors fail
 * closed, including during the middle of a packet. */
static int transfer(mqtt_feed_t *feed, void *bytes, size_t size, bool send, int64_t until) {
  size_t done = 0;
  while (done < size && feed->current(feed->context)) {
    if (clock_ms() >= until) return done ? -1 : 0;
    uint8_t *at = (uint8_t *)bytes + done;
    const ssize_t n = send ? esp_tls_conn_write(feed->tls, at, size - done)
                           : esp_tls_conn_read(feed->tls, at, size - done);
    if (n > 0) done += n;
    else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
      vTaskDelay(pdMS_TO_TICKS(10) + 1);
    else return -1;
  }
  return done == size ? 1 : -1;
}

static uint8_t *put_utf8(uint8_t *out, const char *text) {
  const size_t length = strlen(text);
  *out++ = length >> 8;
  *out++ = length;
  memcpy(out, text, length);
  return out + length;
}

static bool send_packet(mqtt_feed_t *feed, uint8_t type, void *body, size_t size) {
  uint8_t header[1 + PET_MQTT_REMAINING_BYTES] = {type};
  const size_t count = pet_mqtt_length_encode(header + 1, size);
  const int64_t deadline = clock_ms() + IO_MS;
  return count && transfer(feed, header, count + 1, true, deadline) == 1 &&
    transfer(feed, body, size, true, deadline) == 1;
}

static bool connect_packet(mqtt_feed_t *feed, const pet_private_secret_t *secret) {
  uint8_t body[360] = {0, 4, 'M', 'Q', 'T', 'T', 4, 2, 0, 30};
  if (secret->user[0]) body[7] |= 128;
  if (secret->token[0]) body[7] |= 64;
  char client_id[26];
  snprintf(client_id, sizeof(client_id), "petui%08lx%08lx",
    (unsigned long)esp_random(), (unsigned long)esp_random());
  uint8_t *end = put_utf8(body + 10, client_id);
  if (secret->user[0]) end = put_utf8(end, secret->user);
  if (secret->token[0]) end = put_utf8(end, secret->token);
  const bool sent = send_packet(feed, 0x10, body, end - body);
  mbedtls_platform_zeroize(body, sizeof(body));
  return sent;
}

static int receive(mqtt_feed_t *feed, char *body, uint8_t *type, size_t *length, int64_t idle_until) {
  const int ready = transfer(feed, type, 1, false, idle_until);
  if (ready != 1) return ready;
  const int64_t until = clock_ms() + IO_MS;
  *length = 0;
  for (unsigned index = 0; index < PET_MQTT_REMAINING_BYTES; ++index) {
    uint8_t digit = 0;
    if (transfer(feed, &digit, 1, false, until) != 1 ||
        !pet_mqtt_length_digit(length, index, digit)) return -1;
    if (!(digit & 128)) return transfer(feed, body, *length, false, until) == 1 ? 1 : -1;
  }
  return -1;
}

static bool accept_publish(const pet_private_feed_t *binding, uint8_t type, const char *body,
    size_t length, pet_mqtt_feed_value_fn accept, void *context) {
  if ((type & 0xfe) != 0x30 || length < 2) return false;
  const size_t topic = pet_mqtt_u16((const uint8_t *)body);
  if (topic > length - 2 || topic != strlen(binding->topic) ||
      memcmp(body + 2, binding->topic, topic)) return false;
  const size_t payload = length - 2 - topic;
  if (payload > PET_WIFI_DATA_BODY_MAX) return false;
  if (payload) accept(body + 2 + topic, payload, context);
  return true;
}

bool pet_mqtt_feed_run(const pet_wifi_endpoint_t *endpoint, const pet_private_feed_t *binding,
                      const pet_private_secret_t *secret, char *workspace,
                      pet_mqtt_feed_current_fn current, pet_mqtt_feed_value_fn accept, void *ctx) {
  const esp_tls_cfg_t config = {
    .non_block = true,
    .timeout_ms = IO_MS,
    .crt_bundle_attach = esp_crt_bundle_attach,
  };
  mqtt_feed_t feed = {
    .tls = (binding->kind == PET_PUBLIC_MQTT ? pet_live_tls_connect_public_port : pet_live_tls_connect_port)(endpoint->host, binding->port, &config,
      clock_ms() + IO_MS, current, ctx),
    .current = current,
    .context = ctx,
  };
  if (!feed.tls) return false;
  bool received = false;
  uint8_t type;
  size_t length;
  if (!connect_packet(&feed, secret) ||
      receive(&feed, workspace, &type, &length, clock_ms() + IO_MS) != 1 ||
      type != 0x20 || length != 2 || workspace[0] || workspace[1]) goto done;

  uint8_t subscribe[PET_PRIVATE_TOPIC_MAX + 5] = {0, 1};
  uint8_t *end = put_utf8(subscribe + 2, binding->topic);
  *end++ = 0; /* QoS 0. The client never publishes or acknowledges a publish. */
  if (!send_packet(&feed, 0x82, subscribe, end - subscribe)) goto done;

  const int64_t subscribe_until = clock_ms() + IO_MS;
  int64_t ping_at = clock_ms() + PING_MS, ping_until = 0;
  bool subscribed = false;
  while (current(ctx)) {
    const int64_t now = clock_ms();
    if ((!subscribed && now >= subscribe_until) || (ping_until && now >= ping_until)) break;
    if (!ping_until && now >= ping_at) {
      if (!send_packet(&feed, 0xc0, NULL, 0)) break;
      ping_until = now + IO_MS;
    }
    const int64_t idle = !subscribed ? subscribe_until : ping_until ? ping_until : ping_at;
    const int result = receive(&feed, workspace, &type, &length, idle);
    if (result < 0) break;
    if (!result) continue;
    if (!subscribed && type == 0x90 && length == 3 && workspace[0] == 0 &&
        workspace[1] == 1 && workspace[2] == 0) {
      subscribed = true;
    } else if (type == 0xd0 && !length && ping_until) {
      ping_until = 0;
      ping_at = clock_ms() + PING_MS;
    } else if (current(ctx) && accept_publish(binding, type, workspace, length, accept, ctx)) {
      /* A broker may deliver retained PUBLISH before its SUBACK. */
      received = true;
      mbedtls_platform_zeroize(workspace, length);
      vTaskDelay(pdMS_TO_TICKS(125));
    } else {
      break;
    }
  }
done:
  mbedtls_platform_zeroize(workspace, PET_MQTT_BODY_LIMIT);
  esp_tls_conn_destroy(feed.tls);
  return received;
}
#endif
