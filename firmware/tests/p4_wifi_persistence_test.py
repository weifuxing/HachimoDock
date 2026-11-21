"""Host tests for the Wi-Fi clock and built-in snapshot persistence."""
from pathlib import Path
import subprocess


def test_wifi_persistence(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    tests = firmware / "tests"
    main = firmware / "main"
    cjson = tests / "vendor/cjson"
    (tmp_path / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
    (tmp_path / "esp_netif_sntp.h").write_text("""
#include <stdbool.h>
#include <sys/time.h>
typedef struct {
  bool wait_for_sync;
  void (*sync_cb)(struct timeval *);
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) ((esp_sntp_config_t){0})
int esp_netif_sntp_init(const esp_sntp_config_t *config);
void esp_netif_sntp_deinit(void);
""")
    (tmp_path / "nvs.h").write_text("""
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef unsigned nvs_handle_t;
typedef int esp_err_t;
int nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_commit(nvs_handle_t);
int nvs_get_i32(nvs_handle_t, const char *, int32_t *);
int nvs_set_i32(nvs_handle_t, const char *, int32_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
""")
    executable = tmp_path / "wifi-persistence"
    clock = tmp_path / "clock_fixture.c"
    clock.write_text("""
#include "pet_p4_wifi_clock.c"

void simulate_clock_reset(void) {
  synced_utc_ms = 0;
  synced_at_ms = 0;
  age_at_anchor_ms = 0;
}
""")
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DPET_P4_SYSTEM_WIFI=1",
        "-D_POSIX_C_SOURCE=200809L", "-Dgettimeofday=fixture_gettimeofday",
        "-I", str(tmp_path), "-I", str(main), "-I", str(cjson),
        "-I", str(tests / "stubs/widget_data"), "-I", str(tests / "stubs/audio_frontend"),
        "-include", "freertos/portmacro.h",
        str(tests / "p4_wifi_persistence_test.c"),
        str(clock), str(main / "pet_p4_wifi_data_cache.c"),
        str(main / "pet_p4_widget_data.c"), str(main / "pet_p4_wifi_data_sources.c"),
        str(cjson / "cJSON.c"), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
