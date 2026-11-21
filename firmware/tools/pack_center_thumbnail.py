"""Encode one 64×48 RGB thumbnail as a 16-color RGB565 palette + 4-bit indices."""
import struct
import sys

from PIL import Image

image = Image.frombytes("RGB", (64, 48), sys.stdin.buffer.read()).quantize(
    colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
)
palette = (image.getpalette() + [0] * 48)[:48]
out = sys.stdout.buffer
for r, g, b in zip(palette[::3], palette[1::3], palette[2::3]):
    out.write(struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | b >> 3))
indices = image.tobytes()
out.write(bytes((a << 4) | b for a, b in zip(indices[::2], indices[1::2])))
