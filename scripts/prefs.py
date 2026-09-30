#!/usr/bin/env python3
"""Read MUI preference files.

    python3 scripts/prefs.py <preset.prefs>

MUI stores a preset as an IFF `FORM` of type `PREF`. Its first chunk, `PRHD`,
is a six-byte version header; its second, `MUIC`, is the settings. Each entry in
`MUIC` is a four-byte big-endian key, a four-byte big-endian length, and that
many bytes of value -- no padding and no terminator, so the parse is exact and
anything left over is an error.

The keys are MUI's own preference-item ids. Keys 55..97 are the 43 standard MUI
images (`MUII_WindowBack` .. `MUII_PopFont` in the SDK's `libraries/mui.h`), so
an image key names its role as `key - 55`; the keys below 55 are the settings the
preferences program shows (fonts, colours, spacing, frames) and the keys above
97 are its private items.

A value's meaning is the item's, not the file's: an image is a path
(`3:XEN/Plain/11pt/CheckMark.mf0`, where the `3:` is an image class and `XEN` is
the drawer under `Images/`), a font is `Name/Size`, a colour is six hex digits,
and a background is a pattern (`2:m1` or `0:135`). This module reads the entries
and hands them back; the meaning is the caller's.

Host tool only; not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

import pins


class PrefsError(Exception):
    """The file is not a MUI preference file, or is truncated."""


# The first MUI image's preference-item id: `MUII_WindowBack` is 0 and its key is
# 55 (the SDK's `libraries/mui.h` lists the `MUII_*` images in this order).
IMAGE_KEY_BASE = 55

# The 43 standard MUI images, in id order (`MUII_*`, mui.h). A key in
# [IMAGE_KEY_BASE, IMAGE_KEY_BASE + len(MUII)) names one of these.
MUII = (
    "WindowBack", "RequesterBack", "ButtonBack", "ListBack", "TextBack",
    "PropBack", "PopupBack", "SelectedBack", "ListCursor", "ListSelect",
    "ListSelCur", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",
    "CheckMark", "RadioButton", "Cycle", "PopUp", "PopFile",
    "PopDrawer", "PropKnob", "Drawer", "HardDisk", "Disk",
    "Chip", "Volume", "RegisterBack", "Network", "Assign",
    "TapePlay", "TapePlayBack", "TapePause", "TapeStop", "TapeRecord",
    "GroupBack", "SliderBack", "SliderKnob", "TapeUp", "TapeDown",
    "PageBack", "ReadListBack", "PopFont",
)


def entries(path: Path) -> list[tuple[int, bytes]]:
    """The (key, value) entries of a MUI preference file, in file order."""
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:4] != b"FORM":
        raise PrefsError("not an IFF FORM")
    form = struct.unpack(">I", data[4:8])[0]
    if form != len(data) - 8:
        raise PrefsError(f"FORM says {form} bytes, the file has {len(data) - 8}")
    if data[8:12] != b"PREF":
        raise PrefsError(f"FORM type is {data[8:12]!r}, not PREF")

    found: list[tuple[int, bytes]] | None = None
    off = 12
    while off + 8 <= len(data):
        tag = data[off : off + 4]
        length = struct.unpack(">I", data[off + 4 : off + 8])[0]
        body = data[off + 8 : off + 8 + length]
        if len(body) != length:
            raise PrefsError(f"chunk {tag!r} runs past the end")
        if tag == b"MUIC":
            found = _entries(body)
        off += 8 + length + (length & 1)
    if found is None:
        raise PrefsError("no MUIC chunk")
    return found


def _entries(body: bytes) -> list[tuple[int, bytes]]:
    out: list[tuple[int, bytes]] = []
    at = 0
    while at < len(body):
        if at + 8 > len(body):
            raise PrefsError("an entry header runs past the end")
        key = struct.unpack(">I", body[at : at + 4])[0]
        length = struct.unpack(">I", body[at + 4 : at + 8])[0]
        value = body[at + 8 : at + 8 + length]
        if len(value) != length:
            raise PrefsError(f"entry {key:#x} runs past the end")
        out.append((key, value))
        at += 8 + length
    return out


def image_name(key: int) -> str | None:
    """The `MUII_*` name a key names, or None when it is not an image item."""
    index = key - IMAGE_KEY_BASE
    if 0 <= index < len(MUII):
        return MUII[index]
    return None


def _print(path: Path) -> int:
    try:
        found = entries(path)
    except (PrefsError, OSError) as problem:
        pins.report(False, f"{path}: {problem}")
        return 1
    for key, value in found:
        name = image_name(key)
        label = f"{key:>3} / {key:#06x}"
        if name is not None:
            label += f"  {name:<13}"
        text = value.rstrip(b"\x00")
        shown = repr(text.decode("ascii")) if text.isascii() else value.hex()
        print(f"{label}  {len(value):>4}  {shown}")
    pins.report(True, f"{path.name}: {len(found)} entries")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.splitlines()[2].strip(), file=sys.stderr)
        return 2
    return _print(Path(argv[1]))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
