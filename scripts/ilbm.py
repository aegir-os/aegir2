#!/usr/bin/env python3
"""Decode an IFF ILBM image to RGBA.

    from ilbm import decode
    width, height, rgba, palette = decode(path)

MUI's XEN theme ships its gadget art as IFF ILBM bitmaps
(`Images/xen/Plain/11pt/ArrowUp.mf0` and friends); they are the theme's sprites,
so the toolkit needs them as plain pixels. The format is the Amiga's: a BMHD
header, a CMAP palette, and a BODY of bitplanes, optionally run-length encoded
(ByteRun1) and optionally carrying a mask plane for transparency. Only what the
artwork uses is implemented -- no HAM, no EHB, no 24-bit ILBM.

Host tool only; not part of any target build.
"""

from __future__ import annotations

import struct
from pathlib import Path

__all__ = ["decode", "IlbmError"]


class IlbmError(Exception):
    pass


def _chunks(data: bytes):
    """Yield (id, body) for each chunk of a FORM's contents."""
    if data[:4] != b"FORM":
        raise IlbmError("not a FORM")
    form = data[8:12]
    if form != b"ILBM":
        raise IlbmError(f"not an ILBM ({form!r})")
    offset = 12
    while offset + 8 <= len(data):
        cid = data[offset : offset + 4]
        size = struct.unpack(">I", data[offset + 4 : offset + 8])[0]
        body = data[offset + 8 : offset + 8 + size]
        offset += 8 + size + (size & 1)  # chunks are word-aligned
        yield cid, body


def _byte_run1(body: bytes) -> bytes:
    """Decompress the ByteRun1 (PackBits) stream a BODY may carry."""
    out = bytearray()
    i = 0
    while i < len(body):
        n = body[i]
        i += 1
        if n < 128:  # a literal run of n+1 bytes
            out += body[i : i + n + 1]
            i += n + 1
        elif n > 128:  # a repeat of the next byte, 257-n times
            out += bytes([body[i]]) * (257 - n)
            i += 1
        # 128 is a no-op
    return bytes(out)


def decode(path: str | Path):
    """Return (width, height, rgba, palette) for an ILBM file.

    `rgba` is `width * height` 4-byte tuples in top-left-first row order, with
    alpha 255 for an opaque pixel and 0 where the mask (or the transparent
    colour) leaves it out. `palette` is the CMAP as (r, g, b) triples.
    """
    data = Path(path).read_bytes()
    bmhd = cmap = body = None
    for cid, chunk in _chunks(data):
        if cid == b"BMHD":
            bmhd = chunk
        elif cid == b"CMAP":
            cmap = chunk
        elif cid == b"BODY":
            body = chunk
    if bmhd is None or body is None:
        raise IlbmError("missing BMHD or BODY")

    width, height = struct.unpack(">HH", bmhd[0:4])
    planes = bmhd[8]
    masking = bmhd[9]
    compression = bmhd[10]
    transparent = struct.unpack(">H", bmhd[12:14])[0]

    if compression == 1:
        body = _byte_run1(body)
    elif compression != 0:
        raise IlbmError(f"compression {compression} is not supported")

    palette = [
        (cmap[i], cmap[i + 1], cmap[i + 2]) for i in range(0, len(cmap), 3)
    ] if cmap else [(0, 0, 0)]

    has_mask = (masking & 1) != 0
    bits_per_row = planes + (1 if has_mask else 0)
    row_bytes = ((width + 15) // 16) * 2  # rows are whole 16-bit words

    pixels: list[tuple[int, int, int, int]] = []
    at = 0
    for _y in range(height):
        index = [0] * width
        mask = [1] * width
        for plane in range(bits_per_row):
            row = body[at : at + row_bytes]
            at += row_bytes
            for x in range(width):
                bit = (row[x // 8] >> (7 - (x % 8))) & 1
                if plane < planes:
                    index[x] |= bit << plane
                else:
                    mask[x] = bit
        for x in range(width):
            i = index[x]
            r, g, b = palette[i] if i < len(palette) else (0, 0, 0)
            opaque = mask[x] if has_mask else (i != transparent if masking == 2 else 1)
            pixels.append((r, g, b, 255 if opaque else 0))

    return width, height, pixels, palette
