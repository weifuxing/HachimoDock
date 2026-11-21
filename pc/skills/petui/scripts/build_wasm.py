#!/usr/bin/env python3
"""Build a bounded wasm32 component and package its validated source directory."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from compile_wasm import compile_wasm
from pack_hachiapp import package_directory, write_package


def compiler(variable, choices):
    selected = os.environ.get(variable)
    if selected:
        return selected
    for name in choices:
        path = shutil.which(name)
        if path:
            return path
    raise ValueError(f"未找到 {choices[-1]}，请安装 LLVM 或设置 {variable}")


def build(code, package, output, clang, linker, checker=None, trace=None, large=False):
    if output.suffix.lower() != ".hachiapp":
        raise ValueError("输出必须为 .hachiapp")
    sdk = Path(__file__).resolve().parents[1] / "sdk"
    with tempfile.TemporaryDirectory() as work:
        wasm = Path(work) / "guest.wasm"
        compile_wasm(code, sdk, clang, linker, wasm, large=large)
        if trace and not checker:
            raise ValueError("回放验收需要固件同源 WAMR 检查器")
        if checker:
            subprocess.run([checker, str(wasm), *([str(trace)] if trace else [])],
                           check=True, timeout=60)
        files, module = package_directory(package, wasm.read_bytes())
        write_package(output, files, module, require_cover=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("code", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--large", action="store_true", help="使用四页固定内存")
    parser.add_argument("--checker", default=os.environ.get("PETUI_WASM_CHECKER"))
    args = parser.parse_args()
    try:
        build(args.code, args.package, args.output,
              compiler("CLANG", ("clang-20", "clang")),
              compiler("WASM_LD", ("wasm-ld-20", "wasm-ld")), args.checker, args.trace, args.large)
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    print(args.output.resolve())


if __name__ == "__main__":
    main()
