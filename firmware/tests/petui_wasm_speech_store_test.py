"""Run the device speech store against bounded fixtures and a real temp filesystem."""
from pathlib import Path
import subprocess
import sys


def test_wasm_speech_sidecar_owns_storage_and_live_generation(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    (tmp_path / "freertos").mkdir()
    (tmp_path / "mbedtls").mkdir()
    (tmp_path / "esp_heap_caps.h").write_text(
        "#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
        "#define heap_caps_malloc(n,c) malloc(n)\n"
    )
    (tmp_path / "esp_log.h").write_text("#define ESP_LOGW(tag,...) ((void)(tag))\n")
    (tmp_path / "freertos/FreeRTOS.h").write_text("#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffu\n")
    (tmp_path / "freertos/semphr.h").write_text(
        "typedef void *SemaphoreHandle_t;\nstatic inline void *xSemaphoreCreateMutex(void){return (void*)1;}\n"
        "#define xSemaphoreTake(a,b) ((void)(a),(void)(b),1)\n#define xSemaphoreGive(a) ((void)(a))\n"
        "#define vSemaphoreDelete(a) ((void)(a))\n"
    )
    (tmp_path / "mbedtls/sha256.h").write_text(
        "#include <openssl/sha.h>\ntypedef SHA256_CTX mbedtls_sha256_context;\n"
        "#define mbedtls_sha256_init(ctx) ((void)(ctx))\n"
        "#define mbedtls_sha256_free(ctx) ((void)(ctx))\n"
        "#define mbedtls_sha256_starts(ctx, n) (!SHA256_Init(ctx))\n"
        "#define mbedtls_sha256_update(ctx, bytes, size) (!SHA256_Update(ctx, bytes, size))\n"
        "#define mbedtls_sha256_finish(ctx, out) (!SHA256_Final(out, ctx))\n"
        "#define mbedtls_sha256(bytes,size,out,n) (SHA256(bytes,size,out)==NULL)\n"
    )
    if sys.platform == 'darwin':
        header = tmp_path / 'mbedtls/sha256.h'
        text = header.read_text().replace('<openssl/sha.h>', '<CommonCrypto/CommonDigest.h>')
        for old, new in [('SHA256_CTX', 'CC_SHA256_CTX'), ('SHA256_Init', 'CC_SHA256_Init'), ('SHA256_Update', 'CC_SHA256_Update'), ('SHA256_Final', 'CC_SHA256_Final'), ('SHA256(bytes', 'CC_SHA256(bytes')]:
            text = text.replace(old, new)
        header.write_text(text)
        crypto = []
    else:
        crypto = subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcrypto"], text=True).split()
    executable = tmp_path / "speech-store"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
         "-Wno-unused-variable", "-fsanitize=address,undefined", "-DPET_P4_WASM_AUDIO=1",
         "-I", str(tmp_path), "-I", str(firmware / "main"), "-I", str(firmware / "tests/vendor/cjson"),
         str(firmware / "tests/petui_wasm_speech_store_test.c"),
         *[str(firmware / "main" / name) for name in
           ("pet_p4_component_ext.c", "pet_p4_component_events.c", "pet_p4_sfx_core.c")],
         str(firmware / "tests/vendor/cjson/cJSON.c"), *crypto, "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], cwd=tmp_path, check=True, timeout=15)
