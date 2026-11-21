#include "pet_p4_live_tls.h"

static pet_wifi_https_stage_t pet_live_tls_failure(esp_tls_t *tls, int result,
                                                          int *error_code) {
  esp_tls_error_handle_t handle = NULL;
  int verify_flags = 0, tls_code = 0;
  esp_err_t error = esp_tls_get_error_handle(tls, &handle) == ESP_OK && handle
    ? esp_tls_get_and_clear_last_error(handle, &tls_code, &verify_flags) : 0;
  *error_code = tls_code ? -tls_code : error;
  if (verify_flags) return PET_WIFI_HTTPS_CERTIFICATE;
  if (error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) return PET_WIFI_HTTPS_DNS;
  if (error == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT ||
      error == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) return PET_WIFI_HTTPS_TCP;
#ifdef ESP_PLATFORM
  esp_tls_conn_state_t state;
  if (result == 0 && esp_tls_get_conn_state(tls, &state) == ESP_OK &&
      state == ESP_TLS_CONNECTING) return PET_WIFI_HTTPS_TCP;
#endif
  return result == 0 ? PET_WIFI_HTTPS_IO : PET_WIFI_HTTPS_TLS;
}

/* One fresh connection after a transient failure. The caller hasn't sent
 * a request yet. Certificate errors and cancelled playback are terminal. */
esp_tls_t *pet_live_tls_connect_port(const char *host, uint16_t port, const esp_tls_cfg_t *config,
                                              int64_t limit, pet_live_tls_current_fn current,
                                              void *context) {
  for (unsigned attempt = 0; attempt < 2 && current(context); ++attempt) {
    const int64_t start = esp_timer_get_time() / 1000;
    if (start >= limit) break;
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return NULL;
    int64_t deadline = start + config->timeout_ms;
    if (deadline > limit) deadline = limit;
    esp_tls_cfg_t step = *config;
    // Include the first TCP retransmission on a lossy connection, without
    // extending the caller's total request budget.
    step.timeout_ms = deadline - start > 3500 ? 3500 : (int)(deadline - start);
    int result = 0;
    while (esp_timer_get_time() / 1000 < deadline && current(context)) {
      result = esp_tls_conn_new_async(host, (int)strlen(host), port, &step, tls);
      if (result != 0) break;
#ifdef ESP_PLATFORM
      // ESP-TLS 5.5 retains the fd sets that select() clears on a TCP timeout.
      // A fresh attempt can make progress; polling that socket again cannot.
      esp_tls_conn_state_t state;
      if (esp_tls_get_conn_state(tls, &state) == ESP_OK && state == ESP_TLS_CONNECTING) break;
#endif
      vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
    if (result == 1 && current(context) && esp_timer_get_time() / 1000 < limit) return tls;
    int error;
    const pet_wifi_https_stage_t stage = pet_live_tls_failure(tls, result, &error);
    const bool retry = stage == PET_WIFI_HTTPS_TCP || stage == PET_WIFI_HTTPS_DNS ||
      (result == 0 && stage == PET_WIFI_HTTPS_IO);
    esp_tls_conn_destroy(tls);
    if (result != 1 && current(context)) pet_device_wifi_note_https(stage, error);
    if (!retry) break;
    if (attempt == 0 && current(context)) vTaskDelay(pdMS_TO_TICKS(250) + 1);
  }
  return NULL;
}

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "pet_p4_wifi_endpoint.h"

esp_tls_t *pet_live_tls_connect_public_port(const char *host, uint16_t port,
    const esp_tls_cfg_t *config, int64_t deadline, pet_live_tls_current_fn current,
    void *context) {
  const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *results = NULL;
  if (!current(context)) return NULL;
  if (getaddrinfo(host, NULL, &hints, &results) != 0) {
    if (current(context)) pet_device_wifi_note_https(PET_WIFI_HTTPS_DNS, 0);
    return NULL;
  }
  char ip[INET_ADDRSTRLEN] = {0};
  for (const struct addrinfo *it = results; it; it = it->ai_next) {
    if (it->ai_family != AF_INET || it->ai_addrlen < sizeof(struct sockaddr_in)) continue;
    const struct sockaddr_in *address = (const struct sockaddr_in *)it->ai_addr;
    if (pet_wifi_endpoint_public_ipv4(ntohl(address->sin_addr.s_addr)) &&
        inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip))) break;
  }
  freeaddrinfo(results);
  if (!ip[0]) {
    if (current(context)) pet_device_wifi_note_https(PET_WIFI_HTTPS_DNS, 0);
    return NULL;
  }
  esp_tls_cfg_t public_config = *config;
  public_config.common_name = host;
  public_config.skip_common_name = false;
  public_config.cacert_buf = NULL;
  public_config.cacert_bytes = 0;
  public_config.crt_bundle_attach = esp_crt_bundle_attach;
  return pet_live_tls_connect_port(ip, port, &public_config, deadline, current, context);
}
#endif
