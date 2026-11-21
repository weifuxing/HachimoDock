"""Exercise the generated ESP-Hosted INIT handler for the common C5/C6 image."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from patch_hosted_radio import patch_transport


def test_common_sdio_handshake_identifies_c5_and_c6(tmp_path):
    default = ROOT / 'managed_components/espressif__esp_hosted/host/drivers/transport/transport_drv.c'
    vendor = Path(os.environ.get('PET_P4_HOSTED_TRANSPORT_SOURCE', default))
    if not vendor.is_file():
        pytest.skip('Requires the pinned ESP-Hosted checkout used for the firmware build')
    source = patch_transport(vendor.read_bytes())
    start = source.index('static bool verify_host_config_for_slave(')
    verify = source[start:source.index('\n}', start) + 2]
    start = source.index('static int process_init_event(uint8_t *evt_buf, uint16_t len)\n{')
    setup = source[start:source.index('\n}', start) + 2]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <inttypes.h>
#define H_TRANSPORT_SDIO 1
#define H_TRANSPORT_SPI 2
#define H_TRANSPORT_IN_USE H_TRANSPORT_SDIO
#define H_SLAVE_TARGET_ESP32C6 1
#define TAG ""
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x) == 0)
#define H_TEST_RAW_TP_DIR 0
#define H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD 0
#define H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD 0
enum { ESP_FAIL=-1, ESP_ERR_INVALID_SIZE=1, ESP_ERR_NOT_SUPPORTED=2 };
enum { ESP_PRIV_FIRMWARE_CHIP_ESP32C6=0x0d, ESP_PRIV_FIRMWARE_CHIP_ESP32C5=0x17,
       ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED=0xff };
enum { ESP_PRIV_CAPABILITY=1, ESP_PRIV_CAP_EXT, ESP_PRIV_FIRMWARE_CHIP_ID, ESP_PRIV_TEST_RAW_TP,
       ESP_PRIV_RX_Q_SIZE, ESP_PRIV_TX_Q_SIZE, ESP_PRIV_FIRMWARE_VERSION, ESP_PRIV_TRANS_SDIO_MODE };
enum { TRANSPORT_TX_ACTIVE=1 };
static atomic_uint chip_type = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;
static unsigned activated, configured;
static int get_chip_str_from_id(uint8_t id, char *name) {(void)id; name[0]=0; return 0;}
static void check_if_max_freq_used(uint8_t id) {(void)id;}
static void process_capabilities(uint8_t n) {(void)n;}
static void print_capabilities(uint8_t n) {(void)n;}
static uint32_t process_ext_capabilities(const uint8_t *p) {(void)p; return 0;}
static void print_ext_capabilities(const uint8_t *n) {(void)n;}
static void compare_fw_version(uint32_t n) {(void)n;}
static void transport_driver_event_handler(uint8_t n) {assert(n==1); activated++;}
static unsigned send_slave_config(unsigned cap, unsigned id, unsigned test, unsigned low, unsigned high) {
    assert(!cap && !test && !low && !high);
    configured=id;
    return 0;
}
static void transport_delayed_init(void) {}
''' + verify + '\n' + setup + r'''
int main(void) {
    uint8_t init[] = {ESP_PRIV_FIRMWARE_CHIP_ID,1,0x0d};
    assert(process_init_event(init, sizeof(init))==0);
    assert(activated==1 && configured==0x0d && atomic_load(&chip_type)==0x0d);
    init[2]=0x17;
    assert(process_init_event(init, sizeof(init))==0);
    assert(activated==2 && configured==0x17 && atomic_load(&chip_type)==0x17);
    init[2]=0x08;
    assert(process_init_event(init, sizeof(init))==ESP_ERR_NOT_SUPPORTED);
    assert(activated==2);
    return 0;
}
'''
    src = tmp_path / 'hosted.c'
    src.write_text(harness)
    binary = tmp_path / 'hosted'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_dual_band_is_requested_only_for_detected_c5(tmp_path):
    source = (ROOT / 'main/pet_p4_device_wifi.c').read_text()
    start = source.index('static bool start_radio(void)')
    radio_start = source[start:source.index('\n}', start) + 2]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_HOSTED_EVENT 1
#define ESP_HOSTED_EVENT_TRANSPORT_FAILURE 1
#define WIFI_EVENT 2
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT 3
#define IP_EVENT_STA_GOT_IP 3
#define IP_EVENT_STA_LOST_IP 4
#define WIFI_IF_STA 0
#define WIFI_MODE_STA 1
#define WIFI_STORAGE_RAM 1
#define WIFI_BAND_MODE_AUTO 3
#define HOSTED_CHIP_ESP32C5 0x17u
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
typedef int esp_err_t;
typedef struct { int nvs_enable; } wifi_init_config_t;
typedef int wifi_config_t;
typedef struct { unsigned major1, minor1, patch1; } esp_hosted_coprocessor_fwver_t;
static atomic_uint hosted_probe_step;
static bool started, dual_band, verified;
static struct { unsigned radio_version; int band_mode_error; bool dual_band; } link_status;
static unsigned detected, band_calls;
static int band_result;
static int esp_netif_init(void) {return ESP_OK;}
static int esp_event_loop_create_default(void) {return ESP_OK;}
static bool esp_netif_create_default_wifi_sta(void) {return true;}
static int esp_hosted_init(void) {return ESP_OK;}
static int esp_wifi_init(const wifi_init_config_t *p) {assert(!p->nvs_enable);return ESP_OK;}
static int esp_wifi_set_storage(int n) {assert(n==WIFI_STORAGE_RAM);return ESP_OK;}
static int esp_wifi_set_mode(int n) {assert(n==WIFI_MODE_STA);return ESP_OK;}
static void wifi_event(void) {}
static int esp_event_handler_register(int base, int id, void (*event)(void), void *arg) {
    (void)base; (void)id; (void)event; (void)arg; return ESP_OK;
}
static unsigned pet_p4_hosted_radio_chip_id(void) {return detected;}
static int esp_wifi_set_band_mode(int mode) {assert(mode==WIFI_BAND_MODE_AUTO);band_calls++;return band_result;}
static int esp_wifi_set_config(int iface, const wifi_config_t *p) {assert(iface==WIFI_IF_STA && !*p);return ESP_OK;}
static int esp_wifi_start(void) {return ESP_OK;}
static int esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *p) {
    *p=(esp_hosted_coprocessor_fwver_t){2,12,11};return ESP_OK;
}
static void pet_p4_finish_board_probe(bool ok) {verified=ok;}
static void ble_budget_probe(void) {assert(started);}
''' + radio_start + r'''
int main(int argc, char **argv) {
    assert(argc==3);
    detected=(unsigned)strtoul(argv[1],NULL,0);
    band_result=atoi(argv[2]);
    assert(start_radio() && started && verified);
    assert(band_calls==(detected==HOSTED_CHIP_ESP32C5));
    assert(link_status.dual_band==(detected==HOSTED_CHIP_ESP32C5 && !band_result));
    assert(link_status.band_mode_error==(detected==HOSTED_CHIP_ESP32C5 ? band_result : 0));
    return 0;
}
'''
    src = tmp_path / 'band.c'
    src.write_text(harness)
    binary = tmp_path / 'band'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    for chip, result in [('0x0d', '0'), ('0x17', '0'), ('0x17', '12')]:
        subprocess.run([str(binary), chip, result], check=True)
