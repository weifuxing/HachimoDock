"""Optional local-Python driver for the pinned PetUI C toolchain."""
from pathlib import Path
import subprocess
import sys


def compile_wasm(code, sdk, clang, linker, wasm, large=False):
    obj = wasm.with_suffix(".o")
    subprocess.run([clang, "--no-default-config", "--target=wasm32-unknown-unknown",
                    "-mcpu=mvp", "-I", sdk, "-std=c11", "-Os", "-Wall", "-Wextra",
                    "-Werror", "-ffreestanding", "-fno-builtin", "-c", code, "-o", obj],
                   check=True, timeout=60)
    for memory in ([262144] if large else [131072, 262144]):
        result = subprocess.run([linker, "--no-entry", "--export-memory", "--strip-all",
                                 f"--initial-memory={memory}", f"--max-memory={memory}", "-z",
                                 "stack-size=16384", obj, "-o", wasm],
                                stderr=subprocess.PIPE, text=True, timeout=30)
        if result.returncode == 0:
            return
        if memory != 131072 or "initial memory too small" not in result.stderr:
            raise ValueError(result.stderr.strip() or f"WASM 链接失败（{result.returncode}）")


if __name__ == "__main__":
    compile_wasm(*map(Path, sys.argv[1:]))
