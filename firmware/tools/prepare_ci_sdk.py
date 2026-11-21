"""Prepare real, separate v1/v3 SDK contexts for the full host test suite.

Uses the project's locked PlatformIO environments. No device discovery, upload,
fixture substitutions, ignored test selection or runtime acceptance claims.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen

from patch_hosted_radio import patch_transport

# Same immutable source and archive digest as pc/scripts/prepare-petui-runtime.mjs.
WAMR_REVISION = "47c7c85ac21f240a878026af603e032065d183fb"
WAMR_SHA256 = "fb5b7bd3f9d09e882463fe126314925af748c1b84caaea83ac15dea830602cef"
WAMR_URL = f"https://codeload.github.com/bytecodealliance/wasm-micro-runtime/tar.gz/{WAMR_REVISION}"
PROJECT_INPUTS = (
    "sdkconfig.defaults", "sdkconfig.v3.defaults", "sdkconfig.wifi-settings.defaults",
    "config/wifi6-ble.defaults", "dependencies.lock", "dependencies.v3.lock",
    "dependencies.wifi-v1.lock", "dependencies.wifi-v3.lock",
)


def check_checkout_inputs(project):
    # Local ignored defaults can make preparation pass while a clean checkout
    # fails. Require both checked-in inputs and actual files before downloads.
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z", "--", *PROJECT_INPUTS], cwd=project,
    ).decode().split("\0")
    missing = [name for name in PROJECT_INPUTS
               if name not in tracked or not (project / name).is_file()]
    if missing:
        raise SystemExit("Required SDK inputs must be tracked and present: " + ", ".join(missing))


@contextmanager
def preserve_sdkconfig(project, target):
    # ESP-IDF rewrites this input during configure. Keep the actual generated
    # headers/database, but restore the reviewed source input byte-for-byte so
    # later source-profile checks see the checkout, not SDK-normalized output.
    path = project / f"sdkconfig.{target}"
    original = path.read_bytes() if path.exists() else None
    try:
        yield
    finally:
        if original is None:
            path.unlink(missing_ok=True)
        else:
            path.write_bytes(original)


def prepare_wamr(cache, archive=None):
    """Always extract verified bytes; a cached extracted tree is not trusted."""
    cache.mkdir(parents=True, exist_ok=True)
    supplied = archive is not None
    archive = Path(archive) if supplied else cache / f"{WAMR_REVISION}.tar.gz"
    if not archive.is_file():
        if supplied:
            raise SystemExit("Supplied WAMR archive is missing")
        with tempfile.TemporaryDirectory(dir=cache) as temporary:
            download = Path(temporary) / "download"
            with urlopen(WAMR_URL, timeout=60) as response, download.open("wb") as output:
                size = 0
                while block := response.read(65536):
                    size += len(block)
                    if size > 32 * 1024 * 1024:
                        raise SystemExit("WAMR archive exceeds the bounded download size")
                    output.write(block)
            if hashlib.sha256(download.read_bytes()).hexdigest() != WAMR_SHA256:
                raise SystemExit("WAMR archive SHA-256 mismatch")
            download.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != WAMR_SHA256:
        raise SystemExit("WAMR archive SHA-256 mismatch; remove the invalid cache explicitly")
    name = f"wasm-micro-runtime-{WAMR_REVISION}"
    destination = cache / name
    if destination.is_symlink():
        raise SystemExit("WAMR output must not be a symlink")
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        with tarfile.open(archive, "r:gz") as source:
            if any(Path(member.name).parts[0] != name for member in source.getmembers()):
                raise SystemExit("Unexpected WAMR archive root")
            source.extractall(temporary, filter="data")
        extracted = Path(temporary) / name
        if not (extracted / "build-scripts/runtime_lib.cmake").is_file():
            raise SystemExit("WAMR runtime CMake input is missing")
        if destination.exists():
            shutil.rmtree(destination)
        extracted.rename(destination)
    return destination.resolve()


def check_database(database, project):
    entries = json.loads(database.read_text())
    for name in ("pet_p4_storage_io_trace.c", "pet_p4_spiffs_hint.c"):
        source = (project / "main" / name).resolve()
        matches = [entry for entry in entries if Path(entry["file"]).resolve() == source]
        if len(matches) != 1:
            raise SystemExit(f"ESP-IDF database must contain {name} exactly once")
        if not source.is_file() or not Path(matches[0]["directory"]).is_dir():
            raise SystemExit("SDK production source/build context is missing")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--v3-core", required=True)
    parser.add_argument("--input-cache", type=Path)
    parser.add_argument("--wamr-archive", type=Path, help="Use an existing hash-verified archive without downloading WAMR")
    args = parser.parse_args()
    github_env = os.environ.get("GITHUB_ENV")
    if not github_env:
        raise SystemExit("GITHUB_ENV is required to export the prepared test inputs")
    project = Path(__file__).resolve().parents[1]
    check_checkout_inputs(project)
    cores = {"v1": Path.home() / ".platformio", "v3": Path(args.v3_core).resolve()}
    if cores["v1"].resolve() == cores["v3"]:
        raise SystemExit("v1 and v3 require separate SDK directories")
    cache = (args.input_cache or cores["v3"].parent / "p4-ci-inputs").resolve()
    wamr = prepare_wamr(cache, args.wamr_archive)
    exports = {"WAMR_ROOT": str(wamr)}

    def configure(family, target):
        env = {**os.environ, "PLATFORMIO_CORE_DIR": str(cores[family]), "WAMR_ROOT": str(wamr)}
        with preserve_sdkconfig(project, target):
            subprocess.run([sys.executable, "-m", "platformio", "run", "--project-dir", str(project),
                            "--environment", target, "--target", "compiledb", "--disable-auto-clean"], env=env, check=True)

    for family, core in cores.items():
        target = "esp32_p4_program_public_candidate" + ("_v3" if family == "v3" else "")
        configure(family, target)
        database = project / ".pio/build" / target / "compile_commands.json"
        check_database(database, project)
        cc = core / "packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-gcc"
        sdk = core / "packages/framework-espidf"
        if not cc.is_file() or not (sdk / "tools/cmake/version.cmake").is_file():
            raise SystemExit("Prepared SDK/compiler is incomplete")
        exports[f"PET_P4_{family.upper()}_COMPILE_COMMANDS"] = str(database)
        exports[f"PET_{family.upper()}_SDK_ROOT"] = str(sdk)
        exports[f"PET_{family.upper()}_SDK_CC"] = str(cc)
        exports[f"PET_P4_{family.upper()}_IDF_PATH"] = str(sdk)
    # The non-WiFi profiles do not resolve ESP-Hosted. Configure the locked
    # WiFi profile last so managed_components retains the required superset.
    configure("v3", "esp32_p4_wifi6_system_settings_v3")
    hosted = project / "managed_components/espressif__esp_hosted/host/drivers/transport/transport_drv.c"
    patch_transport(hosted.read_bytes())  # Existing exact production source hash guard.
    exports["PET_P4_HOSTED_TRANSPORT_SOURCE"] = str(hosted)
    exports["PET_SPIFFS_COMPILE_COMMANDS"] = exports["PET_P4_V3_COMPILE_COMMANDS"]
    for family in cores:
        database = Path(exports[f"PET_P4_{family.upper()}_COMPILE_COMMANDS"])
        check_database(database, project)
    with open(github_env, "a", encoding="utf-8") as stream:
        for name, value in exports.items():
            if "\n" in value or "\r" in value:
                raise SystemExit("Invalid SDK path")
            stream.write(f"{name}={value}\n")
    print("Prepared WAMR, both real SDK contexts, both NimBLE inputs and pinned ESP-Hosted source.")


if __name__ == "__main__":
    main()
