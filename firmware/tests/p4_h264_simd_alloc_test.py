"""Exercise the real tinyH264-only allocator with RTC/L2/PSRAM heap admission."""
from pathlib import Path
import atexit
import subprocess
import runpy
import os
import shutil
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
ALLOCATOR = ROOT / 'main'


def test_decoder_allocations_exclude_rtc_and_preserve_fallbacks(tmp_path):
    (tmp_path / 'esp_heap_caps.h').write_text('''
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 4u
#define MALLOC_CAP_SPIRAM 1024u
#define MALLOC_CAP_INTERNAL 2048u
#define MALLOC_CAP_DEFAULT 4096u
#define MALLOC_CAP_SIMD (1u << 20)
void *heap_caps_malloc(size_t, uint32_t);
void *heap_caps_malloc_prefer(size_t, size_t, ...);
void *heap_caps_calloc_prefer(size_t, size_t, size_t, ...);
''')
    (tmp_path / 'test.c').write_text(r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_h264_simd_alloc.c"
static unsigned char rtc[64], l2[64], psram[64];
static int l2_available = 1, psram_available = 1;
static uint32_t seen[3];
static size_t calls, preferences;
static void *allocate(size_t size, uint32_t caps) {
    if (!size || size > 64) return NULL;
    /* Ordinary INTERNAL requests can land in RTC: reproduces the admission bug. */
    if (!(caps & MALLOC_CAP_SIMD)) return rtc;
    if (caps & MALLOC_CAP_INTERNAL) return l2_available ? l2 : NULL;
    if (caps & MALLOC_CAP_SPIRAM) return psram_available ? psram : NULL;
    return l2_available ? l2 : (psram_available ? psram : NULL);
}
void *heap_caps_malloc(size_t size, uint32_t caps) {
    ++calls; seen[0] = caps; preferences = 1;
    return allocate(size, caps);
}
static void *prefer(size_t size, size_t num, va_list args) {
    ++calls; preferences = num;
    for (size_t i = 0; i < num; ++i) seen[i] = va_arg(args, uint32_t);
    for (size_t i = 0; i < num; ++i) {
        void *p = allocate(size, seen[i]);
        if (p || !size) return p;
    }
    return NULL;
}
void *heap_caps_malloc_prefer(size_t size, size_t num, ...) {
    va_list args; va_start(args, num);
    void *p = prefer(size, num, args); va_end(args); return p;
}
void *heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...) {
    va_list args; va_start(args, num);
    void *p = prefer(n * size, num, args); va_end(args);
    if (p) memset(p, 0, n * size);
    return p;
}
int main(void) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    assert(heap_caps_malloc(32, internal) == rtc);
    assert(pet_h264_simd_malloc_prefer(32, 2, internal, external) == l2);
    assert(preferences == 2 && seen[0] == (internal | MALLOC_CAP_SIMD));
    assert(seen[1] == (external | MALLOC_CAP_SIMD));
    l2_available = 0;
    assert(pet_h264_simd_malloc_prefer(32, 2, internal, external) == psram);
    assert(pet_h264_simd_malloc_prefer(32, 1, internal) == NULL);
    memset(psram, 0xa5, sizeof(psram));
    assert(pet_h264_simd_calloc_prefer(4, 8, 2, internal, external) == psram);
    for (unsigned i = 0; i < 32; ++i) assert(psram[i] == 0);
    assert(psram[32] == 0xa5);
    l2_available = 1;
    assert(pet_h264_simd_malloc_prefer(32, 2, external, internal) == psram);
    psram_available = 0;
    assert(pet_h264_simd_malloc_prefer(32, 2, external, internal) == l2);
    assert(pet_h264_simd_malloc_prefer(32, 3, external, external, internal) == l2);
    assert(pet_h264_simd_malloc(32) == l2);
    assert(seen[0] == (MALLOC_CAP_DEFAULT | MALLOC_CAP_SIMD));
    l2_available = 0;
    assert(!pet_h264_simd_malloc_prefer(32, 2, internal, external));
    assert(!pet_h264_simd_calloc_prefer(4, 8, 2, internal, external));
    assert(!pet_h264_simd_malloc(32));
    assert(!pet_h264_simd_malloc_prefer(0, 2, internal, external));
    size_t before = calls;
    assert(!pet_h264_simd_calloc_prefer(SIZE_MAX, 2, 1, internal));
    assert(!pet_h264_simd_malloc_prefer(32, 0));
    assert(!pet_h264_simd_malloc_prefer(32, 4));
    assert(calls == before);
    /* Other users of the system heap still have access to RTC. */
    assert(heap_caps_malloc(32, internal) == rtc);
}
''')
    binary = tmp_path / 'simd-alloc'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(tmp_path), '-I', str(ALLOCATOR),
                    str(tmp_path / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_allocator_is_selected_after_component_resolution_and_archive_selection():
    cmake = (ROOT / 'CMakeLists.txt').read_text()
    selector = 'include("${CMAKE_CURRENT_LIST_DIR}/tools/h264_pre_v3.cmake")'
    hook = 'include("${CMAKE_CURRENT_LIST_DIR}/tools/h264_simd.cmake")'
    assert cmake.index('project(pet_manager_p4_runtime)') < cmake.index(selector) < cmake.index(hook)
    patch = (ROOT / 'tools/h264_simd.cmake').read_text()
    assert '"$ENV{PET_P4_SYSTEM_WIFI}" STREQUAL "1"' in patch
    assert 'sw/libs/esp32p4_less_v3/libtinyh264.a' in patch
    assert 'sw/libs/esp32p4/libtinyh264.a' in patch
    assert 'get_target_property(h264_original tinyh264' not in patch
    assert 'list(INSERT h264_links ${h264_link_index} "${h264_fixed}")' in patch
    assert 'target_sources(${h264_app} PRIVATE' in patch
    for name in ('malloc', 'heap_caps_malloc_prefer', 'heap_caps_calloc_prefer'):
        assert f'--wrap={name}' not in (ROOT / 'platformio.ini').read_text()


def test_archive_patcher_rejects_unknown_input_without_changing_output(tmp_path):
    patch = runpy.run_path(str(ROOT / 'tools/patch_h264_simd.py'))['patch']
    source = tmp_path / 'vendor.a'
    output = tmp_path / 'patched.a'
    source.write_bytes(b'unreviewed vendor archive')
    output.write_bytes(b'previous complete archive')
    with pytest.raises(ValueError, match='Unreviewed'):
        patch(source, output, 'must-not-run', 'must-not-run', 'must-not-run')
    with pytest.raises(ValueError, match='immutable'):
        patch(source, source, 'must-not-run', 'must-not-run', 'must-not-run')
    assert source.read_bytes() == b'unreviewed vendor archive'
    assert output.read_bytes() == b'previous complete archive'


@pytest.mark.parametrize(('family', 'rev_max', 'expected_error'), [
    ('v1', 199, None),
    ('v3', 399, None),
    ('pico', 199, None),
    ('v1', 399, 'must target pre-v3 P4 silicon'),
    ('v1', 199, 'Cannot select the pre-v3 P4 openh264 archive'),
])
def test_project_configure_selects_silicon_archives(tmp_path, family, rev_max, expected_error):
    cmake = shutil.which('cmake')
    if not cmake:
        pytest.skip('CMake is needed for project hookup regression')
    tools = tmp_path / 'tools'
    tools.mkdir()
    for name in ('h264_pre_v3.cmake', 'h264_simd.cmake'):
        shutil.copyfile(ROOT / 'tools' / name, tools / name)
    (tmp_path / 'main').mkdir()
    shutil.copyfile(ROOT / 'main/pet_p4_h264_simd_alloc.c', tmp_path / 'main/pet_p4_h264_simd_alloc.c')
    component = tmp_path / 'component'
    component.mkdir()
    for profile in ('esp32p4_less_v3', 'esp32p4'):
        lib = component / 'sw/libs' / profile / 'libtinyh264.a'
        lib.parent.mkdir(parents=True)
        lib.write_bytes(profile.encode())
        if not (profile == 'esp32p4_less_v3' and expected_error and 'openh264' in expected_error):
            (lib.parent / 'libopenh264.a').write_bytes(profile.encode())
    # The archive transform is separately verified against both real vendor
    # libraries. Here test CMake scope/path/order without compiling an SDK.
    (tools / 'patch_h264_simd.py').write_text('''
import sys, pathlib
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
pathlib.Path(args['--output']).write_bytes(pathlib.Path(args['--source']).read_bytes())
''')
    (component / 'CMakeLists.txt').write_text('''
add_library(tinyh264 STATIC IMPORTED)
set_target_properties(tinyh264 PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/sw/libs/esp32p4/libtinyh264.a")
add_library(openh264 STATIC IMPORTED)
set_target_properties(openh264 PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/sw/libs/esp32p4/libopenh264.a")
add_library(h264 INTERFACE)
target_link_libraries(h264 INTERFACE tinyh264 openh264)
''')
    (tmp_path / 'dummy.c').write_text('void dummy(void) {}\n')
    (tmp_path / 'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.16)
project(h264_hook C)
set(IDF_TARGET esp32p4)
set(CONFIG_ESP32P4_REV_MAX_FULL "''' + str(rev_max) + '''")
set(PYTHON "''' + sys.executable + '''")
add_subdirectory(component)
if(TARGET tinyh264)
  message(FATAL_ERROR "Fixture must reproduce IDF's non-GLOBAL imported target")
endif()
add_library(app STATIC dummy.c)
function(idf_component_get_property out component property)
  if(property STREQUAL "COMPONENT_DIR")
    set(${out} "${CMAKE_SOURCE_DIR}/component" PARENT_SCOPE)
  elseif(component STREQUAL "main")
    set(${out} app PARENT_SCOPE)
  else()
    set(${out} h264 PARENT_SCOPE)
  endif()
endfunction()
include(tools/h264_pre_v3.cmake)
include(tools/h264_simd.cmake)
get_target_property(links h264 INTERFACE_LINK_LIBRARIES)
file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${links}")
''')
    env = dict(os.environ, PET_P4_SYSTEM_WIFI='0' if family == 'pico' else '1',
               PET_P4_CHIP_FAMILY='v1' if family == 'pico' else family)
    build = tmp_path / 'build'
    configure = subprocess.run([cmake, '-S', str(tmp_path), '-B', str(build)], env=env,
                               capture_output=True, text=True)
    if expected_error:
        assert configure.returncode != 0
        assert expected_error in configure.stderr
        return
    configure.check_returncode()
    tiny, openh264 = (build / 'selected.txt').read_text().split(';')
    if family == 'pico':
        assert (tiny, openh264) == ('tinyh264', 'openh264')
        assert not (tmp_path / '.pio').exists()
    else:
        selected_path = Path(tiny)
        assert selected_path.is_absolute() and 'managed_components' not in tiny
        assert not selected_path.is_relative_to(build)
        profile = 'esp32p4_less_v3' if family == 'v1' else 'esp32p4'
        assert selected_path.read_bytes() == profile.encode()
        assert openh264 == (str(component / 'sw/libs' / profile / 'libopenh264.a')
                           if family == 'v1' else 'openh264')


def test_platformio_applies_compatible_linker_only_to_wifi6_v1(tmp_path, monkeypatch):
    class SconsEnvironment(dict):
        def subst(self, _):
            return str(tmp_path)

        def GetProjectOption(self, _, default):
            return default

        def Replace(self, **kwargs):
            self.update(kwargs)

        def Append(self, **kwargs):
            self.update(kwargs)

        def AddPostAction(self, *_):
            pass

        def PioPlatform(self):
            return SimpleNamespace(get_package_dir=lambda _: None)

    def configure(target):
        env = SconsEnvironment(PIOENV=target, ENV={})
        runpy.run_path(str(ROOT / 'scripts/platformio_add_p4_toolchain.py'),
                       init_globals={'env': env, 'Import': lambda _: None})
        return env

    monkeypatch.setattr(atexit, 'register', lambda *args: None)
    monkeypatch.delenv('PET_P4_PRE_V3_LINKER_DIR', raising=False)
    legacy = 'esp32_p4_wifi6_system_settings_v1'
    with pytest.raises(RuntimeError, match='PET_P4_PRE_V3_LINKER_DIR'):
        configure(legacy)
    linker = tmp_path / 'ld'
    linker.write_text('#!/bin/sh\n')
    linker.chmod(0o755)
    monkeypatch.setenv('PET_P4_PRE_V3_LINKER_DIR', str(tmp_path))
    assert configure(legacy)['LINKFLAGS'] == [f'-B{tmp_path}/']
    for target in ('esp32_p4_wifi6_system_settings_v3', 'esp32_p4_pico_v1'):
        assert 'LINKFLAGS' not in configure(target)
