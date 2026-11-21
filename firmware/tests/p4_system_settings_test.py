"""Exercise the actual Wi-Fi and LAN settings pages with fake radio and pairing state."""
from pathlib import Path
import subprocess


def test_local_wifi_settings_keyboard_and_navigation(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    tests = firmware / "tests"
    executable = tmp_path / "system-settings-test"
    (tmp_path / "mbedtls").mkdir()
    (tmp_path / "mbedtls/platform_util.h").write_text("#include <stddef.h>\nvoid mbedtls_platform_zeroize(void *, size_t);\n")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DPET_P4_SYSTEM_WIFI=1",
         "-I", str(tmp_path), "-I", str(firmware / "main"), "-I", str(tests / "audio_volume_stubs"),
         "-I", str(tests / "vendor/cjson"),
         str(firmware / "main/pet_p4_system_settings.c"),
         str(tests / "p4_system_settings_test.c"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
