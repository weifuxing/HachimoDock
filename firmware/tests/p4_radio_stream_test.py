"""Exercise the real radio HTTP, MP3 and PCM pipeline with an in-memory TLS peer."""
import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_radio_allowlist_matches_bundled_https_mp3_catalog():
    path = ROOT.parent / "pc/builtin-clawpkgs/city-radio/pc/catalog.json"
    stations = sorted(json.loads(path.read_text()), key=lambda item: -item["rank"])
    expected = []
    for station in stations:
        channel = re.fullmatch(r"https://lhttp\.qingting\.fm/live/(\d{1,12})/64k\.mp3", station["url"])
        if channel and station["available"] and not station["hls"]:
            expected.append([station["id"], station["name"], channel[1]])
    source = (ROOT / "main/pet_p4_radio_catalog.inc").read_text()
    actual = [json.loads("[" + line.strip()[1:-2] + "]")
              for line in source.splitlines() if line.startswith("  {")]
    assert actual == expected
    assert len(actual) >= 12


def test_radio_stream_chunked_mp3_timeout_and_format_rejection(tmp_path):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        pytest.skip("ffmpeg generates the synthetic MP3")
    stubs = {
        "esp_heap_caps.h": """
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_calloc(n,s,c) calloc(n,s)
#define heap_caps_free(p) free(p)
""",
        "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "esp_tls.h": """
#pragma once
#include <sys/types.h>
#define ESP_OK 0
typedef int esp_err_t;
typedef int *esp_tls_error_handle_t;
typedef struct { int placeholder; } esp_tls_t;
typedef struct {
  bool non_block; int timeout_ms; const uint8_t *cacert_buf; size_t cacert_bytes;
  const char *common_name; bool skip_common_name; esp_err_t (*crt_bundle_attach)(void *);

} esp_tls_cfg_t;
typedef enum { ESP_TLS_CONNECTING } esp_tls_conn_state_t;
esp_err_t esp_tls_get_conn_state(esp_tls_t *, esp_tls_conn_state_t *);
esp_tls_t *esp_tls_init(void);
esp_err_t esp_tls_get_error_handle(esp_tls_t *, esp_tls_error_handle_t *);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t, int *, int *);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
void esp_tls_conn_destroy(esp_tls_t *);
""",
        "esp_crt_bundle.h": "esp_err_t esp_crt_bundle_attach(void *);\n",
        "lwip/netdb.h": "#include <netdb.h>\n",
        "lwip/inet.h": "#include <arpa/inet.h>\n",
        "esp_tls_errors.h": """
#define ESP_TLS_ERR_SSL_WANT_READ -1
#define ESP_TLS_ERR_SSL_WANT_WRITE -2
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 10
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST 11
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 12
""",
        "freertos/FreeRTOS.h": "#define portMAX_DELAY 0\n#define pdTRUE 1\n#define pdPASS 1\n#define pdMS_TO_TICKS(x) (x)\n",
        "freertos/queue.h": """
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(int, size_t);
int xQueueReceive(QueueHandle_t, void *, int);
int xQueueOverwrite(QueueHandle_t, const void *);
void vQueueDelete(QueueHandle_t);
""",
        "freertos/task.h": """
int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *);
void vTaskDelay(int);
void vTaskDelete(void *);
""",
    }
    for name, source in stubs.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
    fixture = tmp_path / "tone.mp3"
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                    "sine=frequency=440:sample_rate=44100:duration=2", "-ac", "2", "-b:a", "64k",
                    "-write_xing", "0", "-id3v2_version", "0", str(fixture)], check=True)
    finite_mp3, finite_wav = tmp_path / "finite.mp3", tmp_path / "finite.wav"
    for target, options in (
        (finite_mp3, ["-b:a", "64k", "-write_xing", "0", "-id3v2_version", "0"]),
        (finite_wav, ["-c:a", "pcm_s16le"]),
    ):
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                        "sine=frequency=440:sample_rate=16000:duration=0.25",
                        "-ac", "2", *options, str(target)], check=True)
    exe = tmp_path / "radio-stream-test"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-DPET_P4_SYSTEM_WIFI=1", "-DESP_PLATFORM", "-D_POSIX_C_SOURCE=200809L", "-I", str(tmp_path), "-I", str(ROOT / "main"),
                    str(ROOT / "tests/p4_radio_stream_test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(fixture), str(finite_mp3), str(finite_wav)], check=True, timeout=10)
