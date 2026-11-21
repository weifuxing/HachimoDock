"""Compile real renderer replay functions against an intentionally mutating decoder."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    returns = "void" if name.startswith("reset_h264_decoder") else "bool"
    start = source.index("static " + returns + " " + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_mutating_decoder_cannot_damage_cached_stream():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    declarations = source[source.index("static esp_h264_dec_handle_t g_h264_decoder;"):source.index("static uint8_t *g_h264_ppa_input;")]
    functions = "\n".join(function(source, name) for name in [
        "reset_h264_decoder", "reset_h264_decoder_traced", "reset_h264_decoder_measured", "start_h264_decoder_role", "decode_next_h264_frame_role", "start_h264_decoder", "decode_next_h264_frame",
    ])
    harness = (ROOT / "tests/p4_h264_replay_test.c").read_text()
    with tempfile.TemporaryDirectory() as tmp:
        test = Path(tmp) / "replay.c"
        binary = Path(tmp) / "replay"
        test.write_text(harness.replace("/* RENDERER_IMPLEMENTATION */", declarations + '\n' + functions))
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(ROOT / "main"), str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_h264_render_uses_private_replay_buffer():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    decode = function(source, "decode_next_h264_frame_role")
    assert 'decode_next_h264_frame_role(width,height,true,&g_last_decode_us,NULL)' in function(source,'decode_next_h264_frame')
    assert ".buffer = g_h264_work_stream + g_h264_stream_offset" in decode
    assert "(uint8_t *) (stream +" not in decode
    assert "start_h264_decoder(fs_path, stream->bytes, stream->size)" in source
    assert "decode_next_h264_frame(asset->width, asset->height)" in source
    start = function(source, "start_h264_decoder_role")
    assert 'start_h264_decoder_role(fs_path,stream,stream_size,&g_render_trace,NULL)' in function(source,'start_h264_decoder')
    assert "stream_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES" in start
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in start


def test_wifi6_targets_use_same_scalar_deblocking_and_serial_decode(tmp_path):
    from configparser import ConfigParser
    config = ConfigParser(interpolation=None, strict=False)
    config.read(ROOT / 'platformio.ini')
    for board_family in ('v1', 'v3'):
        section = config[f'env:esp32_p4_wifi6_system_settings_{board_family}']
        assert '${wifi6_h264_decode.build_flags}' in section['build_flags'].split()
    flags = config['wifi6_h264_decode']['build_flags'].split()
    for flag in ('H264_SCALAR_HOR_LUMA', 'H264_SERIAL_DECODE', 'H264_PANIC_TRACE'):
        assert f'-DPET_P4_{flag}=1' in flags
    for symbol in ('h264bsdAlloc', 'espFilterHorLuma_bS123', 'espFilterHorLumaEdge',
                   'espFilterHorLumaCross_bS123', 'espFilterHorLumaEdgeCross'):
        assert f'-Wl,--wrap={symbol}' in flags

    renderer = (ROOT / 'main/pet_p4_renderer.c').read_text()
    assert '#if PET_P4_H264_SCALAR_HOR_LUMA\n#include "pet_p4_h264_deblock.inc"' in renderer
    source = tmp_path / 'luma.c'
    source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define IRAM_ATTR
#include "pet_p4_h264_deblock.inc"

enum { STRIDE = 21, ROWS = 6 };
static uint8_t image[ROWS * STRIDE];
static uint8_t split_p[3 * STRIDE], split_q[3 * STRIDE];
static uint32_t alpha = 20, beta = 12;

static void reset(void) {
  const uint8_t samples[ROWS] = {100, 104, 102, 112, 110, 115};
  for (unsigned i = 0; i < STRIDE; ++i) {
    for (unsigned row = 0; row < ROWS; ++row) image[row * STRIDE + i] = samples[row];
    for (unsigned row = 0; row < 3; ++row) {
      split_p[row * STRIDE + i] = samples[row];
      split_q[row * STRIDE + i] = samples[3 + row];
    }
  }
}

static void check(const uint8_t *p, const uint8_t *q, unsigned start, unsigned count) {
  const uint8_t original[ROWS] = {100, 104, 102, 112, 110, 115};
  const uint8_t filtered[ROWS] = {100, 103, 106, 108, 111, 115};
  for (unsigned i = 0; i < STRIDE; ++i) {
    const uint8_t *expected = (i >= start && i < start + count) ? filtered : original;
    for (unsigned row = 0; row < 3; ++row) {
      assert(p[row * STRIDE + i] == expected[row]);
      assert(q[row * STRIDE + i] == expected[row + 3]);
    }
  }
}

int main(void) {
  reset();
  __wrap_espFilterHorLuma_bS123(&image[3 * STRIDE + 1], &alpha, &beta, 2, STRIDE);
  check(image, image + 3 * STRIDE, 1, 16);
  reset();
  __wrap_espFilterHorLumaEdge(&image[3 * STRIDE + 3], &alpha, &beta, 2, STRIDE);
  check(image, image + 3 * STRIDE, 3, 4);
  reset();
  __wrap_espFilterHorLumaCross_bS123(split_q + 2, &split_p[2 * STRIDE + 2], &alpha, &beta, 2, STRIDE);
  check(split_p, split_q, 2, 16);
  reset();
  __wrap_espFilterHorLumaEdgeCross(split_q + 5, &split_p[2 * STRIDE + 5], &alpha, &beta, 2, STRIDE);
  check(split_p, split_q, 5, 4);
  reset();
  alpha = 10;
  __wrap_espFilterHorLuma_bS123(&image[3 * STRIDE + 1], &alpha, &beta, 2, STRIDE);
  check(image, image + 3 * STRIDE, 0, 0);
  reset();
  alpha = 20;
  beta = 2;
  __wrap_espFilterHorLuma_bS123(&image[3 * STRIDE + 1], &alpha, &beta, 2, STRIDE);
  check(image, image + 3 * STRIDE, 0, 0);
  beta = 12;
  const uint8_t opposite[ROWS] = {115, 110, 112, 102, 104, 100};
  const uint8_t opposite_filtered[ROWS] = {115, 111, 108, 106, 103, 100};
  for (unsigned row = 0; row < ROWS; ++row) image[row * STRIDE] = opposite[row];
  __wrap_espFilterHorLumaEdge(image + 3 * STRIDE, &alpha, &beta, 2, STRIDE);
  for (unsigned row = 0; row < ROWS; ++row) assert(image[row * STRIDE] == opposite_filtered[row]);
  const uint8_t black_edge[ROWS] = {0, 8, 0, 0, 0, 0};
  for (unsigned row = 0; row < ROWS; ++row) image[row * STRIDE] = black_edge[row];
  __wrap_espFilterHorLumaEdge(image + 3 * STRIDE, &alpha, &beta, 2, STRIDE);
  assert(image[2 * STRIDE] == 1 && image[3 * STRIDE] == 0);
}
''')
    binary = tmp_path / 'luma'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'main'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_wifi6_v3_allocator_forces_serial_decode(tmp_path):
    renderer = (ROOT / 'main/pet_p4_renderer.c').read_text()
    start = renderer.index('extern h264bsd_hd_t __real_h264bsdAlloc')
    wrapper = renderer[start:renderer.index('#endif', start)]
    source = tmp_path / 'serial.c'
    source.write_text(r'''
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
typedef void *h264bsd_hd_t;
typedef struct {uint32_t dualTaskEnable, dualTaskCore, dualTaskPriority;} h264bsd_cfg_t;
''' + wrapper + r'''
static unsigned calls;
static h264bsd_cfg_t original = {1, 1, 17};
h264bsd_hd_t __real_h264bsdAlloc(h264bsd_cfg_t *config) {
  ++calls;
  if (!config) return NULL;
  assert(config != &original);
  assert(config->dualTaskEnable == 0 && config->dualTaskCore == 1 && config->dualTaskPriority == 17);
  return &original;
}
int main(void) {
  assert(__wrap_h264bsdAlloc(&original) == &original && original.dualTaskEnable == 1);
  assert(__wrap_h264bsdAlloc(NULL) == NULL && calls == 2);
}
''')
    binary = tmp_path / 'serial'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
