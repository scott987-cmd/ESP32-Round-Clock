#!/usr/bin/env python3
"""Convert an image into the clock's fixed 466x466 RGB565LE payload."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps


SIZE = (466, 466)
EXPECTED_BYTES = SIZE[0] * SIZE[1] * 2


def convert(source: Path, destination: Path) -> None:
    with Image.open(source) as opened:
        image = ImageOps.fit(opened.convert("RGB"), SIZE, Image.Resampling.LANCZOS)
    image = ImageEnhance.Contrast(image).enhance(1.04)

    payload = bytearray(EXPECTED_BYTES)
    offset = 0
    pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
    for red, green, blue in pixels:
        rgb565 = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        struct.pack_into("<H", payload, offset, rgb565)
        offset += 2

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)
    print(f"wrote {destination} ({len(payload)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    convert(args.source, args.destination)


if __name__ == "__main__":
    main()
