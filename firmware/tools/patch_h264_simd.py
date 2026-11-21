#!/usr/bin/env python3
"""Redirect only tinyH264's allocation references; retain opaque ASM members intact."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

RENAMES = {
    'heap_caps_malloc_prefer': 'pet_h264_simd_malloc_prefer',
    'heap_caps_calloc_prefer': 'pet_h264_simd_calloc_prefer',
    'malloc': 'pet_h264_simd_malloc',
}
# esp_h264 1.5.0, respectively esp32p4_less_v3 and esp32p4. These bind the
# reviewed allocator ABI (at most two preferences) and opaque assembly inputs.
ARCHIVE_SHA256 = {
    '02e08464804f7cf98da8fe6e5891b1b9f35552c326500a06b6de2fb4852cab98',
    '3b6781bcd905c78d3361c92a0eba733f1796a3766fde2f07c0904762b6426e9d',
}


def patch(source, output, ar, nm, objcopy):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source == output:
        raise ValueError('The vendor archive must remain immutable')
    if hashlib.sha256(source.read_bytes()).hexdigest() not in ARCHIVE_SHA256:
        raise ValueError('Unreviewed tinyH264 archive; review SIMD allocator ABI')
    members = subprocess.check_output([ar, 't', str(source)], text=True).splitlines()
    if len(set(members)) != len(members):
        raise ValueError('Duplicate tinyH264 archive members')
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        tmp = Path(temporary)
        patched = tmp / 'libtinyh264_simd.a'
        shutil.copyfile(source, patched)
        found = set()
        for member in members:
            if Path(member).name != member:
                raise ValueError('Unexpected archive member path')
            original = subprocess.check_output([ar, 'p', str(source), member])
            obj = tmp / member
            obj.write_bytes(original)
            undefined = {line.split()[-1] for line in subprocess.check_output(
                [nm, '-u', str(obj)], text=True).splitlines() if line.split()}
            names = sorted(undefined & RENAMES.keys())
            if not names:
                continue
            found.update(names)
            command = [objcopy]
            for name in names:
                command += ['--redefine-sym', name + '=' + RENAMES[name]]
            subprocess.run(command + [str(obj)], check=True)
            subprocess.run([ar, 'r', str(patched), str(obj)], check=True,
                           stdout=subprocess.DEVNULL)
        if found != RENAMES.keys():
            raise ValueError('tinyH264 allocator ABI changed; review the archive')
        before = subprocess.check_output([nm, '-A', '-u', str(source)], text=True).replace(str(source), 'ARCHIVE')
        after = subprocess.check_output([nm, '-A', '-u', str(patched)], text=True).replace(str(patched), 'ARCHIVE')
        for name, replacement in RENAMES.items():
            before = before.replace(' U ' + name + '\n', ' U ' + replacement + '\n')
        if sorted(before.splitlines()) != sorted(after.splitlines()):
            raise ValueError('Unexpected tinyH264 symbol change')
        # Atomic publication prevents a failed configure from selecting half a library.
        patched.replace(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('source', 'output', 'ar', 'nm', 'objcopy'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    patch(args.source, args.output, args.ar, args.nm, args.objcopy)
