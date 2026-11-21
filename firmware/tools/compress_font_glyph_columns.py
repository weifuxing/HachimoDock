"""Lossless column packing of the original resident A1 fallback bitmap.

Only the native32 startup parent uses this asset: its validated full metrics are
already available before bitmap decode. Other build profiles retain the original
packed bitmap. No glyph, metric, mapping or runtime rendering changes.
"""
from pathlib import Path
import argparse
import re
import zlib
from compress_font_asset import bitmap_bytes

ROOT = Path(__file__).resolve().parents[1]
STRIP_WIDTH = 4


def transform(raw, reverse=False):
    source = (ROOT / 'main/pet_p4_font_cn.c').read_text()
    rows = re.findall(r'\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*\d+,\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+)', source)
    assert len(rows) == 21208
    out = bytearray(raw)
    for offset, width, height in rows:
        offset, width, height = int(offset), int(width), int(height)
        bits = width * height
        size = (bits + 7) // 8
        assert size <= 34 and offset + size <= len(raw)
        if not size:
            continue
        glyph = f"{int.from_bytes(raw[offset:offset + size], 'big'):0{size * 8}b}"
        # Store four adjacent columns together. The last strip can be narrower.
        # Each strip occupies exactly its original number of bits.
        order = [row * width + x
                 for left in range(0, width, STRIP_WIDTH)
                 for row in range(height)
                 for x in range(left, min(left + STRIP_WIDTH, width))]
        if reverse:
            restored = ['0'] * bits
            for src, dst in enumerate(order):
                restored[dst] = glyph[src]
            pixels = ''.join(restored)
        else:
            pixels = ''.join(glyph[index] for index in order)
        # The tail bits are part of the original bitmap, including padding.
        out[offset:offset + size] = int(pixels + glyph[bits:], 2).to_bytes(size, 'big')
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    raw = bitmap_bytes()
    columns = transform(raw)
    assert transform(columns, reverse=True) == raw
    asset = ROOT / 'assets/fonts/pet_cn_16_glyph_columns.deflate'
    if args.check:
        packed = asset.read_bytes()
    else:
        from zopfli.zlib import compress
        packed = compress(columns, numiterations=15)
    assert zlib.decompress(packed) == columns
    fnv = 2166136261
    for byte in raw:
        fnv = ((fnv ^ byte) * 16777619) & 0xffffffff
    header = ('/* Generated lossless glyph columns; canonical MiSans data remains unchanged. */\n'
              '#pragma once\n'
              f'#define PET_FONT_GLYPH_STRIP_WIDTH {STRIP_WIDTH}U\n'
              f'#define PET_FONT_GLYPH_COLUMNS_PACKED_BYTES {len(packed)}U\n'
              f'#define PET_FONT_GLYPH_COLUMNS_ADLER UINT32_C(0x{zlib.adler32(columns):08x})\n'
              f'#define PET_FONT_GLYPH_COLUMNS_RESTORED_FNV UINT32_C(0x{fnv:08x})\n')
    target = ROOT / 'main/pet_p4_font_glyph_columns_size.h'
    if args.check:
        assert target.read_text() == header, 'Stale column-packed font asset'
    else:
        asset.write_bytes(packed)
        target.write_text(header)
    print(f'Lossless resident font: {len(raw)} -> {len(packed)} bytes; full bitmap restored exactly')


if __name__ == '__main__':
    main()
