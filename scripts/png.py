#!/usr/bin/env python3
"""Decode a non-interlaced 8-bit PNG to RGBA.

    from png import decode
    width, height, pixels = decode(path)

The theme's sprites are PNGs (the imported MUI artwork); the generator that
embeds them runs in the build, whose environment has no Pillow, so it needs a
dependency-free reader. Only what our importer emits is supported: 8-bit
RGB/RGBA, no palette, no interlacing.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _unfilter(raw: bytes, width: int, height: int, bpp: int) -> bytearray:
    stride = width * bpp
    out = bytearray()
    previous = bytearray(stride)
    at = 0
    for _row in range(height):
        filter_type = raw[at]
        at += 1
        line = bytearray(raw[at : at + stride])
        at += stride
        if filter_type == 1:  # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                up_left = previous[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, previous[i], up_left)) & 0xFF
        out += line
        previous = line
    return out


def decode(path: str | Path):
    """Return (width, height, pixels); pixels are (r, g, b, a) top-left first."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    offset = 8
    width = height = depth = color_type = interlace = None
    idat = bytearray()
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        kind = data[offset + 4 : offset + 8]
        chunk = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _comp, _filt, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
    if depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise ValueError("only non-interlaced 8-bit RGB/RGBA PNGs")
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(bytes(idat))
    flat = _unfilter(raw, width, height, channels)
    pixels = []
    for i in range(0, len(flat), channels):
        if channels == 4:
            pixels.append((flat[i], flat[i + 1], flat[i + 2], flat[i + 3]))
        else:
            pixels.append((flat[i], flat[i + 1], flat[i + 2], 255))
    return width, height, pixels
