#!/usr/bin/env python3
"""Package a PetUI directory and optional WASM module as one .hachiapp archive."""
import argparse
import os
import subprocess
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib
import tempfile
import zipfile

FORMAT = "hachiapp/1"
COVER = "pc/cover.png"
COVER_MAX = 256 * 1024


def validate_cover(data):
    if not (33 < len(data) <= COVER_MAX and data[:16] == b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR"):
        raise ValueError("封面必须是 PNG 且不超过 256 KiB")
    if struct.unpack(">II", data[16:24]) not in {(320, 240), (640, 480)}:
        raise ValueError("封面尺寸必须为 320×240 或 640×480")
    offset = 8
    saw_pixels = False
    while offset + 12 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        end = offset + 12 + size
        if end > len(data):
            break
        kind = data[offset+4:offset+8]
        crc = struct.unpack_from(">I", data, end-4)[0]
        if zlib.crc32(data[offset+4:end-4]) != crc:
            break
        saw_pixels |= kind == b"IDAT"
        if kind == b"IEND" and not size and end == len(data) and saw_pixels:
            return
        offset = end
    raise ValueError("封面 PNG 数据损坏")
REQUIRED = ("component.json", "negative-screen.json", "buttons.json",
            "runtime/widget.json", "share.json")
UNSAFE = {".bat",".cmd",".com",".css",".dll",".dylib",".exe",".html",".js",
          ".mjs",".cjs",".ps1",".py",".sh",".so",".svg",".wasm"}


def json_bytes(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def package_directory(source, module=None):
    if not source.is_dir() or source.is_symlink():
        raise ValueError("组件源必须是本地普通目录")
    files = {}
    total = 0
    for path in sorted(source.rglob("*")):
        name = path.relative_to(source).as_posix()
        if path.is_symlink() or ":" in name or "\\" in name:
            raise ValueError(f"不安全的组件路径: {name}")
        if path.is_dir():
            continue
        if not path.is_file() or path.suffix.lower() in UNSAFE and name != "runtime/app.wasm":
            raise ValueError(f"组件内不允许该文件: {name}")
        length = path.stat().st_size
        total += length
        if len(files) >= 120 or length > 4*1024*1024 or total > 7*1024*1024:
            raise ValueError("组件超过打包大小或文件数量上限")
        files[name] = path.read_bytes()
    for name in REQUIRED:
        if name not in files:
            raise ValueError(f"缺少 {name}")
    if module is None:
        module = files.get("runtime/app.wasm")
    return files, module


MAX_WASM_BYTES = 192 * 1024
CANVAS_PROFILE = "640x480-v1"
HOST_PROFILE = "hachi-host-v1"
AUDIO_PROFILE = "hachi-audio-v2"
AUDIO_ASSET = "assets/speech.hab"
HOST_IMPORTS = {b"clock_read", b"uv_read", b"random_u32"}
WASM_IMPORTS = HOST_IMPORTS | {
    b"canvas640", b"text", b"rect", b"blit", b"sprite", b"tone", b"sample", b"load", b"save", b"media_cmd"
}


def wasm_profile(module):
    """Return the profiles selected by imports."""
    if not 8 <= len(module) <= MAX_WASM_BYTES or not module.startswith(b"\0asm\x01\0\0\0"):
        raise ValueError("WASM 模块格式不正确或超过 192 KiB")
    at = 8

    def u32(end):
        nonlocal at
        value = 0
        for shift in range(0, 29, 7):
            if at >= end:
                break
            byte = module[at]
            at += 1
            if shift == 28 and byte > 15:
                break
            value |= (byte & 127) << shift
            if byte < 128:
                return value
        raise ValueError("WASM 区段编码无效")

    def name(end):
        nonlocal at
        length = u32(end)
        if length > end - at:
            raise ValueError("WASM 导入名称越界")
        start = at
        at += length
        return module[start:at]

    imports = set()
    saw_imports = saw_memory = False
    while at < len(module):
        kind = module[at]
        at += 1
        length = u32(len(module))
        end = at + length
        if kind == 8 or end > len(module):
            raise ValueError("WASM 区段无效或含有 start")
        if kind == 2:
            count = u32(end)
            if saw_imports or count > len(WASM_IMPORTS):
                raise ValueError("WASM 导入过多")
            saw_imports = True
            for _ in range(count):
                namespace, function = name(end), name(end)
                if namespace != b"hachi" or at == end or module[at] != 0 or function not in WASM_IMPORTS or function in imports:
                    raise ValueError("WASM 导入了未开放或重复的宿主接口")
                imports.add(function)
                at += 1
                u32(end)
            if at != end:
                raise ValueError("WASM 导入区段长度无效")
        if kind == 5:
            if saw_memory or module[at:end] not in (bytes((1, 1, 2, 2)), bytes((1, 1, 4, 4))):
                raise ValueError("WASM 必须使用固定的两页或四页内存")
            saw_memory = True
        at = end
    if not saw_memory or b"text" in imports and b"canvas640" not in imports:
        raise ValueError("WASM 缺少固定内存或原生画布声明")
    canvas = CANVAS_PROFILE if b"canvas640" in imports else None
    services = HOST_PROFILE if imports & HOST_IMPORTS else None
    audio = AUDIO_PROFILE if b"sample" in imports else None
    return canvas, services, audio


def validate_audio(data):
    error = ValueError("WASM 语音资源格式、数量或时长无效")
    if data is None or not 16 <= len(data) <= 512 * 1024 or data[:4] != b"HAB1":
        raise error
    count, = struct.unpack_from("<I", data, 4)
    cursor = 8 + 8 * count
    if not 1 <= count <= 64 or cursor > len(data):
        raise error
    for index in range(count):
        offset, length = struct.unpack_from("<II", data, 8 + 8 * index)
        if offset != cursor or not 166 <= length <= 166 * 400 or length % 166 or length > len(data) - cursor:
            raise error
        for at in range(cursor, cursor + length, 166):
            if data[at+2] > 88 or data[at+3:at+6] != b"\0\x40\x01" or data[at+165] & 0xf0:
                raise error
        cursor += length
    if cursor != len(data):
        raise error


def native_command(arguments):
    """Use the installed client's contract, including its firmware interpreter."""
    configured = os.environ.get("PETUI_NATIVE_EXECUTABLE")
    if configured:
        desktop = Path(configured)
    else:
        marker = Path.home() / ".claw-pet/authoring/desktop-path"
        if not marker.is_file():
            raise ValueError("Open Component Center to prepare the current PetUI tools")
        desktop = Path(marker.read_text(encoding="utf-8").strip())
    try:
        result = subprocess.run([str(desktop), "--petui", "tool-info"], capture_output=True, text=True, timeout=15)
        if result.returncode or json.loads(result.stdout).get("protocol", 0) < 2:
            raise ValueError("Update Pet Manager and open Component Center before packaging")
        result = subprocess.run([str(desktop), "--petui", *map(str, arguments)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise ValueError(result.stderr.strip() or "Native package validation failed")
        return result.stdout.strip()
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        raise ValueError(f"PetUI native validation unavailable: {error}") from error


def native_validate(path):
    native_command(["validate", path])


def write_package(destination, files, module=None, *, require_cover=False):
    if COVER in files:
        validate_cover(files[COVER])
    elif require_cover:
        raise ValueError(f"交付包缺少封面 {COVER}")
    info = json.loads(files["component.json"])
    component_id = info["id"]
    if not re.fullmatch(r"[a-z][a-z0-9_-]{0,46}", component_id):
        raise ValueError("组件 ID 不合法")
    manifest = {"format": FORMAT, "id": component_id}
    share = json.loads(files["share.json"])
    if not isinstance(share, dict):
        raise ValueError("share.json 必须是对象")
    title = share.get("title", "")
    name = info.get("name")
    if isinstance(title, str) and not title.strip() and isinstance(name, str) and name.strip():
        share["title"] = name
        files["share.json"] = json_bytes(share)
    widget = json.loads(files["runtime/widget.json"])
    if not isinstance(widget, dict):
        raise ValueError("runtime/widget.json 必须是对象")
    media = widget.get("media")
    if isinstance(media, dict) and media.get("source") == "audio.player":
        raise ValueError("audio.player 已停用。请使用 PetUI 本地媒体服务桥（local-media）创作。")
    surface = widget.get("surface")
    if isinstance(surface, dict) and "services" in surface:
        from app_services import application_errors
        from service_bridge import surface_errors
        declaration = files.get("pc/application.json")
        if declaration is None:
            raise ValueError("服务桥组件缺少 pc/application.json")
        errors = surface_errors(surface, widget) + application_errors(json.loads(declaration), widget)
        if errors:
            raise ValueError("；".join(errors))
    # Match the native runtime's compatible container defaults.
    widget.setdefault("vars", {})
    widget.setdefault("transitions", [])
    states = widget.get("states")
    transitions = widget.get("transitions", [])
    if not isinstance(transitions, list):
        raise ValueError("runtime/widget.json.transitions 必须是数组")
    for index, rule in enumerate(transitions):
        source = rule.get("from") if isinstance(rule, dict) else None
        if source == "*" or (isinstance(source, str) and source and
                             isinstance(states, list) and source in states):
            continue
        raise ValueError(
            f"runtime/widget.json.transitions[{index}].from 必须是已声明的状态或 '*'"
        )
    if module is not None:
        canvas, services, audio = wasm_profile(module)
        for key, profile in (("hachiCanvas", canvas), ("hachiHost", services), ("hachiAudio", audio)):
            if profile:
                widget[key] = profile
            else:
                widget.pop(key, None)
        if audio:
            bank = files.get(AUDIO_ASSET)
            validate_audio(bank)
            widget["hachiAudioSha256"] = hashlib.sha256(bank).hexdigest()
        else:
            widget.pop("hachiAudioSha256", None)
        if not audio and AUDIO_ASSET in files:
            raise ValueError("WASM 模块未使用包内语音资源")
        widget["hachiWasm"] = hashlib.sha256(module).hexdigest()
        files["runtime/widget.json"] = json_bytes(widget)
        files["runtime/app.wasm"] = module
    elif AUDIO_ASSET in files or any(key in widget for key in ("hachiWasm", "hachiCanvas", "hachiHost", "hachiAudio", "hachiAudioSha256")):
        raise ValueError("声明了 WASM 但包内缺少 runtime/app.wasm")
    files["runtime/widget.json"] = json_bytes(widget)
    files["hachiapp.json"] = json_bytes(manifest)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as scratch:
        staged = Path(scratch) / destination.name
        with zipfile.ZipFile(staged, "w") as archive:
            assets = zipfile.ZipInfo("assets/")
            assets.external_attr = (0o40755 << 16) | 0x10
            archive.writestr(assets, b"")
            for name, data in sorted(files.items()):
                archive.writestr(zipfile.ZipInfo(name), data)
        native_validate(staged)
        staged.replace(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--wasm", type=Path)
    args = parser.parse_args()
    if args.output.suffix != ".hachiapp":
        parser.error("输出必须为 .hachiapp")
    if args.wasm and args.wasm.stat().st_size > MAX_WASM_BYTES:
        parser.error("WASM 模块超过 192 KiB")
    try:
        files, module = package_directory(args.source, args.wasm.read_bytes() if args.wasm else None)
        write_package(args.output, files, module, require_cover=True)
    except (ValueError, OSError, KeyError) as exc:
        parser.error(str(exc))
    print(args.output.resolve())


if __name__ == "__main__":
    main()
