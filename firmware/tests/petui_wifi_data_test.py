"""Host test for the same bounded provider and snapshot code used by WiFi6."""
import hashlib
import os
import re
from pathlib import Path
import subprocess
import tempfile

FIRMWARE = Path(__file__).resolve().parents[1]


def test_wifi_data_sources_and_cache():
    cjson = FIRMWARE / "tests/vendor/cjson"
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        (out / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        (out / "nvs.h").write_text("""
#include <stddef.h>
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
typedef int esp_err_t;
int nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_commit(nvs_handle_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
""")
        (out / "esp_heap_caps.h").write_text("""
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free free
""")
        binary = out / "wifi_data_test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-DPET_P4_SYSTEM_WIFI=1", "-Wall", "-Wextra", "-Werror",
            "-I", str(out), "-I", str(FIRMWARE / "main"), "-I", str(cjson),
            "-I", str(FIRMWARE / "tests/stubs/widget_data"),
            str(FIRMWARE / "tests/petui_wifi_data_test.c"),
            str(FIRMWARE / "main/pet_p4_wifi_data_sources.c"),
            str(FIRMWARE / "main/pet_p4_widget_data.c"),
            str(FIRMWARE / "main/pet_p4_wifi_data_cache.c"),
            str(cjson / "cJSON.c"), "-lm", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_wifi_root_der_fingerprints():
    roots = (FIRMWARE / "main/pet_p4_wifi_data_roots.h").read_text()
    expected = {
        "tencent_g5": "371a00dc0533b3721a7eeb40e8419e70799d2b0a0f2c1d80693165f7cec4ad75",
        "hn_r1": "d947432abde7b7fa90fc2e6b59101b1280e0e1c7e4e40fa3c6887fff57a7f4cf",
    }
    for name, fingerprint in expected.items():
        values = roots.split(f"pet_wifi_ca_{name}[] = {{", 1)[1].split("};", 1)[0]
        der = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", values))
        assert hashlib.sha256(der).hexdigest() == fingerprint, name


if __name__ == "__main__":
    test_wifi_data_sources_and_cache()
    test_wifi_root_der_fingerprints()
