#pragma once

#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "esp_timer.h"
#include "pet_p4_device_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

typedef bool (*pet_live_tls_current_fn)(void *);

esp_tls_t *pet_live_tls_connect_port(const char *host, uint16_t port,
    const esp_tls_cfg_t *config, int64_t limit, pet_live_tls_current_fn current,
    void *context);

static inline esp_tls_t *pet_live_tls_connect(const char *host, const esp_tls_cfg_t *config,
                                              int64_t limit, pet_live_tls_current_fn current,
                                              void *context) {
  return pet_live_tls_connect_port(host, 443, config, limit, current, context);
}

#ifdef ESP_PLATFORM
esp_tls_t *pet_live_tls_connect_public_port(const char *host, uint16_t port, const esp_tls_cfg_t *config,
    int64_t deadline, pet_live_tls_current_fn current, void *context);
static inline esp_tls_t *pet_live_tls_connect_public(const char *host, const esp_tls_cfg_t *config,
    int64_t deadline, pet_live_tls_current_fn current, void *context) {
  return pet_live_tls_connect_public_port(host, 443, config, deadline, current, context);
}
#endif
