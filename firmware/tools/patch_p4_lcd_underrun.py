"""Insert the board diagnostic hook in the pinned build-local LCD driver."""
import argparse
import hashlib
from pathlib import Path

PINNED = {
    "3aaeeefa0e7d2e12625942a67d1caf5b9400a5b99d03da0d617aabcc22321125":
        b"    if (unlikely(error_status & MIPI_DSI_LL_EVENT_UNDERRUN)) {\n",
    "6d4a59514c1401334f1a4a89933611b259ca20ef09c9670fabfc37f1dea630da":
        b"    if (intr_status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) {\n",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    original = args.original.read_bytes()
    marker = PINNED.get(hashlib.sha256(original).hexdigest())
    declaration = b'#include "hal/color_hal.h"\n'
    if not marker or original.count(marker) != 1 or original.count(declaration) != 1:
        parser.error("unreviewed ESP-IDF LCD DPI driver")
    generated = original.replace(declaration, declaration + b"\nextern void pet_p4_lcd_on_underrun(void);\n")
    generated = generated.replace(marker, marker + b"        pet_p4_lcd_on_underrun();\n")
    if not args.output.exists() or args.output.read_bytes() != generated:
        args.output.write_bytes(generated)


if __name__ == "__main__":
    main()
