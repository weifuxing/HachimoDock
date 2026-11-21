#!/usr/bin/env python3
"""
[Input] Canonical built-in component manifests, runtime/button JSON, and bounded PNG sprites under pc/.
[Output] Deterministic firmware-embedded component bundle with compiled P4 sprites, or a stale-bundle check.
[Pos] ESP32-P4 build-time generator used by main/CMakeLists.txt.
[Sync] If this file changes, update firmware/.folder.md and protocol.md.
"""

from __future__ import annotations

import argparse
import base64
import json
import hashlib
import os
import re
import sys
import copy
import zlib
from pathlib import Path

from p4_component_sprites import compile_component_sprites, sprite_pack_checksum


RUNTIME_ROOT = Path(__file__).resolve().parents[1]
BUNDLE_PATH = RUNTIME_ROOT / "main" / "pet_p4_builtin_components.json"
DATA_SOURCE_PATH = RUNTIME_ROOT / "main" / "pet_p4_builtin_components_data.c"
# One ordered catalog for factory resources and OTA bootstrap metadata.
COMPONENT_CONFIG = json.loads((RUNTIME_ROOT / "factory-config.json").read_text())["components"]
BUILTIN_IDS = tuple(COMPONENT_CONFIG["ids"])
FACTORY_WASM_IDS = frozenset(COMPONENT_CONFIG.get("factoryWasmIds", ()))

def component_root(component_id: str) -> Path:
    key = "firmwareOnlyPackagesDir" if component_id in COMPONENT_CONFIG.get("firmwareOnlyIds", ()) else "packagesDir"
    return RUNTIME_ROOT / COMPONENT_CONFIG[key] / component_id


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))

def bootstrap_widget(widget: dict) -> tuple[dict, bool]:
    """Artwork is delivered as a resource package, not duplicated in OTA slots.

    Keep the live controls/gameplay for first boot. A factory image/PC sync
    installs the complete package; firmware upgrades must never replace it with
    this image-free bootstrap.
    """
    result = copy.deepcopy(widget)
    surface = result.get("surface", {})
    external = bool(surface.get("images"))
    if external:
        for key in ("images", "fonts", "typography", "interaction"):
            surface.pop(key, None)
        surface["elements"] = [node for node in surface["elements"] if node["type"] != "image"]
        for node in surface["elements"]:
            for key in ("key_for", "show", "tile"):
                node.pop(key, None)
        if "scene_view" in surface:
            mapped={s["entity"] for s in surface["scene_view"]["sprites"]}
            surface["scene_view"]["sprites"]=[]
            for entity in result.get("scene",{}).get("entities",[]):
                if entity["id"] in mapped:entity["shape"]="character"
    return result, external


def read_component_wasm(package: Path, widget: dict) -> bytes | None:
    path = package / "runtime" / "app.wasm"
    expected = widget.get("hachiWasm")
    if expected is None and not path.exists():
        return None
    data = path.read_bytes()
    if not (8 <= len(data) <= 192 * 1024 and data.startswith(b"\x00asm\x01\x00\x00\x00")
            and hashlib.sha256(data).hexdigest() == expected):
        raise ValueError(f"invalid built-in WASM: {path}")
    return data


def render_bundle() -> str:
    if not FACTORY_WASM_IDS.issubset(BUILTIN_IDS):
        raise ValueError("factoryWasmIds must refer to built-in packages")
    components = []
    for component_id in BUILTIN_IDS:
        package = component_root(component_id)
        manifest = read_json(package / "component.json")
        if manifest.get("id") != component_id:
            raise ValueError(f"component id mismatch: {package}")
        widget, external = bootstrap_widget(read_json(package / "runtime" / "widget.json"))
        wasm = read_component_wasm(package, widget)
        if component_id in FACTORY_WASM_IDS and wasm is None:
            raise ValueError(f"factory WASM package has no module: {component_id}")
        sprites = compile_component_sprites(package, widget)
        components.append(
            {
                "id": component_id,
                "version": str(manifest.get("version", "")),
                "resourcePackage": external or wasm is not None,
                "widget": widget,
                "buttons": read_json(package / "buttons.json"),
                "spritesChecksum": f"{sprite_pack_checksum(sprites, wasm):08x}",
                "spriteFiles": [
                    {
                        "id": sprite["id"],
                        "data": base64.b64encode(sprite["data"]).decode("ascii"),
                    }
                    for sprite in sprites
                ],
            }
        )
    return json.dumps(
        {"version": 1, "components": components},
        ensure_ascii=False,
        separators=(",", ":"),
    ) + "\n"


def compressed_arrays(source: str) -> dict[str, bytes]:
    arrays = {}
    pattern = r"\b(pet_p4_builtin_components_zlib|wasm_\w+)\[\] = \{(.*?)\n\};"
    for name, body in re.findall(pattern, source, re.S):
        arrays[name] = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", body))
    return arrays


def matching_stream(encoded: bytes, raw: bytes) -> bool:
    decoder = zlib.decompressobj()
    try:
        decoded = decoder.decompress(encoded, len(raw) + 1)
    except zlib.error:
        return False
    return decoded == raw and decoder.eof and not decoder.unused_data


def render_data_source(bundle: str, previous=None, encoder=None) -> str:
    def encode(name: str, raw: bytes) -> bytes:
        if encoder is not None:
            encoded = encoder(raw)
            if not matching_stream(encoded, raw):
                raise ValueError(f"compressed data failed exact verification: {name}")
            return encoded
        cached = (previous or {}).get(name)
        if cached is not None and matching_stream(cached, raw):
            return cached
        return zlib.compress(raw, 9)

    raw = bundle.encode("utf-8") + b"\0"
    data = json.loads(bundle)
    if data.get("version") != 1 or not 1 <= len(data.get("components", [])) <= 32:
        raise ValueError("invalid built-in index")
    slices = []
    index = []
    prefix = b'{"version":1,"components":['
    if not raw.startswith(prefix):
        raise ValueError("built-in bundle must use canonical serialization")
    cursor = len(prefix)
    for position, package in enumerate(data["components"]):
        if position:
            if raw[cursor:cursor+1] != b',':
                raise ValueError("invalid built-in package separator")
            cursor += 1
        payload = json.dumps(package, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        offset = cursor
        if raw[offset:offset+len(payload)] != payload or not 0 < len(payload) <= 32768:
            raise ValueError("built-in package slice invalid or exceeds 32 KiB")
        slices.append((offset, len(payload)))
        entry = {"id": package["id"], "resourcePackage": package.get("resourcePackage", False)}
        if package["id"] in FACTORY_WASM_IDS:
            entry["factoryWasm"] = True
        index.append(entry)
        cursor = offset + len(payload)
    if raw[cursor:] not in (b']}\0', b']}\n\0'):
        raise ValueError("invalid built-in bundle terminator")
    index_json = json.dumps({"version": 1, "components": index}, ensure_ascii=False, separators=(",", ":"))
    encoded = encode("pet_p4_builtin_components_zlib", raw)
    rows = []
    for offset in range(0, len(encoded), 16):
        chunk = encoded[offset : offset + 16]
        rows.append("  " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    # Identify legacy image-free bootstraps without inflating the bundle at boot
    # or misclassifying legitimate text-only third-party components.
    # Daily News 1.0.1 remains a known image-free bootstrap after adding
    # the optional WiFi headline projection to 1.0.2.
    checks = ['  if (checksum == 0x953a8906U && strcmp(id, "daily-news") == 0) return true;']
    modules, lookup = [], []
    for package in json.loads(bundle)["components"]:
        if package["widget"].get("hachiWasm"):
            # The factory storage image and PC bundle provide the full package.
            # Leave these modules out of the limited OTA application slots.
            if package["id"] in FACTORY_WASM_IDS:
                continue
            wasm = read_component_wasm(component_root(package["id"]), package["widget"])
            name = "wasm_" + package["id"].replace("-", "_")
            compressed = encode(name, wasm)
            modules.append(f"static const uint8_t {name}[] = {{")
            for offset in range(0, len(compressed), 16):
                modules.append("  " + ", ".join(f"0x{b:02x}" for b in compressed[offset:offset+16]) + ",")
            modules.append("};")
            lookup.append(
                f'  if (strcmp(id, "{package["id"]}") == 0) {{ '
                f'*size = sizeof({name}); *decoded = {len(wasm)}; return {name}; }}'
            )
            continue
        if not package.get("resourcePackage", False):
            continue
        value = 2166136261
        for byte in json.dumps(package["widget"], ensure_ascii=False, separators=(",", ":")).encode():
            value = ((value ^ byte) * 16777619) & 0xffffffff
        checks.append(f'  if (checksum == 0x{value:08x}U && strcmp(id, "{package["id"]}") == 0) return true;')
    return "\n".join(
        [
            "/*",
            " * [Input] Generated deterministic JSON from pc/builtin-clawpkgs.",
            " * [Output] Zlib-compressed bounded bootstrap bundle; full art stays in resource packages.",
            " * [Pos] Generated data source linked into the ESP32-P4 application image.",
            " * [Sync] Regenerate with tools/build_builtin_bundle.py; do not edit manually.",
            " */",
            "",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "#include <stdbool.h>",
            "#include <string.h>",
            '#include "pet_p4_builtin_index.h"',
            "const unsigned char pet_p4_builtin_components_zlib[] = {",
            *rows,
            "};",
            f"const size_t pet_p4_builtin_components_zlib_size = {len(encoded)};",
            f"const size_t pet_p4_builtin_components_json_size = {len(raw)};",
            "const char pet_p4_builtin_index_json[] = " + json.dumps(index_json, ensure_ascii=True) + ";",
            "const pet_builtin_slice_t pet_p4_builtin_slices[] = {",
            *[f"  {{{offset}U, {length}U}}," for offset, length in slices],
            "};",
            f"const size_t pet_p4_builtin_slice_count = {len(slices)};",
            "bool pet_p4_builtin_is_bootstrap(const char *id, uint32_t checksum) {",
            "  if (!id) return false;",
            *checks,
            "  return false;",
            "}",
            *modules,
            "const uint8_t *pet_p4_builtin_wasm(const char *id, size_t *size, size_t *decoded) {",
            *lookup,
            "  return NULL;",
            "}",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Build firmware built-in component bundle")
    parser.add_argument("--check", action="store_true", help="fail if the tracked bundle is stale")
    parser.add_argument("--zopfli", action="store_true", help="repack tracked streams with a denser zlib encoder")
    args = parser.parse_args()
    if args.check and args.zopfli:
        parser.error("--check and --zopfli are mutually exclusive")
    current_source = DATA_SOURCE_PATH.read_text(encoding="utf-8") if DATA_SOURCE_PATH.is_file() else ""
    encoder = None
    if args.zopfli:
        from zopfli.zlib import compress

        def encoder(raw):
            return compress(raw, numiterations=15)

    rendered_bundle = render_bundle()
    rendered_source = render_data_source(rendered_bundle, compressed_arrays(current_source), encoder)
    if args.check:
        current_bundle = BUNDLE_PATH.read_text(encoding="utf-8") if BUNDLE_PATH.is_file() else ""
        if current_bundle != rendered_bundle or current_source != rendered_source:
            print(f"stale firmware built-in bundle: {BUNDLE_PATH}", file=sys.stderr)
            return 1
        return 0

    BUNDLE_PATH.parent.mkdir(parents=True, exist_ok=True)
    bundle_temporary = BUNDLE_PATH.with_suffix(".json.tmp")
    source_temporary = DATA_SOURCE_PATH.with_suffix(".c.tmp")
    bundle_temporary.write_text(rendered_bundle, encoding="utf-8")
    source_temporary.write_text(rendered_source, encoding="utf-8")
    os.replace(bundle_temporary, BUNDLE_PATH)
    os.replace(source_temporary, DATA_SOURCE_PATH)
    print(BUNDLE_PATH)
    print(DATA_SOURCE_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
