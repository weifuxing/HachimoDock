#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PET_WIFI_AP_MAX 24
#define PET_WIFI_SSID_SIZE 33
#define PET_WIFI_PASSWORD_SIZE 65

typedef enum {
  PET_WIFI_IDLE, PET_WIFI_SCANNING, PET_WIFI_CONNECTING, PET_WIFI_CONNECTED,
  PET_WIFI_FAILED, PET_WIFI_UNAVAILABLE,
} pet_wifi_status_t;

typedef enum {
  PET_WIFI_OPEN, PET_WIFI_PERSONAL, PET_WIFI_WPA3, PET_WIFI_UNSUPPORTED,
} pet_wifi_security_t;

typedef struct {
  char ssid[PET_WIFI_SSID_SIZE];
  int8_t rssi;
  uint8_t security;
  uint8_t channel;
  uint8_t bssid[6];
} pet_wifi_ap_t;

typedef struct {
  pet_wifi_status_t status;
  uint8_t count;
  uint16_t join_reason;
  bool saved;
  char connected[PET_WIFI_SSID_SIZE];
  pet_wifi_ap_t aps[PET_WIFI_AP_MAX];
} pet_wifi_snapshot_t;

typedef enum { PET_WIFI_SCAN, PET_WIFI_CONNECT, PET_WIFI_FORGET, PET_WIFI_START } pet_wifi_operation_t;

#if PET_P4_SYSTEM_WIFI
typedef enum {
  PET_WIFI_HTTPS_READY = 1, PET_WIFI_HTTPS_DNS, PET_WIFI_HTTPS_TCP,
  PET_WIFI_HTTPS_TLS, PET_WIFI_HTTPS_CERTIFICATE, PET_WIFI_HTTPS_IO,
} pet_wifi_https_stage_t;
typedef enum {
  PET_WIFI_JOIN_NONE, PET_WIFI_JOIN_CONFIG, PET_WIFI_JOIN_START,
  PET_WIFI_JOIN_DISCONNECTED, PET_WIFI_JOIN_ADDRESS_TIMEOUT,
  PET_WIFI_JOIN_AP_MISMATCH, PET_WIFI_JOIN_SAVE, PET_WIFI_JOIN_OK,
} pet_wifi_join_stage_t;
typedef struct {
  unsigned last_stage, successes, failures;
  unsigned join_stage, disconnect_reason;
  unsigned manual_join_stage, manual_disconnect_reason;
  bool manual_legacy_phy;
  unsigned radio_version;
  unsigned connected_channel;
  int band_mode_error;
  bool dual_band;
  unsigned manual_event_channel;
  bool manual_event_matches_ap;
  int join_error;
  unsigned transport_failures;
  uint16_t scan_found, scan_sampled, scan_listed;
  int last_error;
} pet_wifi_link_status_t;

void pet_device_wifi_start(void);
unsigned pet_device_wifi_hosted_probe_step(void);
// ESP-Hosted INIT chip ID, independently of factory board identity.
unsigned pet_p4_hosted_radio_chip_id(void);
bool pet_device_wifi_online(void);
/* Called by the sole authenticated LAN audio owner. Returns the prior policy,
 * or -1 if there is nothing to restore. */
int pet_device_wifi_suspend_power_save(void);
void pet_device_wifi_restore_power_save(int previous);
/* Starts asynchronous station shutdown. True once Wi-Fi is done or unused. */
bool pet_device_wifi_prepare_reboot(void);
void pet_device_wifi_note_https(pet_wifi_https_stage_t stage, int error);
pet_wifi_link_status_t pet_device_wifi_link_status(void);
bool pet_device_wifi_request(pet_wifi_operation_t operation, const pet_wifi_ap_t *ap, const char *password);
void pet_device_wifi_cancel(void);
void pet_device_wifi_snapshot(pet_wifi_snapshot_t *snapshot);
#endif
