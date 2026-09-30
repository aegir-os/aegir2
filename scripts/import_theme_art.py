#!/usr/bin/env python3
"""Convert MUI's XEN theme artwork into PNGs the toolkit can use.

    python3 scripts/import_theme_art.py

Reads the MUI package's `Images/xen/**` ILBM bitmaps and writes them as PNGs
under libs/hosted/aegir-trinket/resources/themes/xen/, mirroring the source
tree. The XEN theme's gadget art is MUI's, so the converted PNGs are committed
as the theme's sprites (the MUI package itself never is); run this again only
when the source art changes. `.image` files are Amiga programmes -- the
scrollbar's drawing is one -- and are skipped.

Host tool only; not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pins
from ilbm import IlbmError, decode

# The artwork suffixes: `.mf0`/`.mf1` are the two frames of a gadget's mark,
# `.mbr` a small bitmap (a volume icon, a cycle button).
SUFFIXES = {".mf0", ".mf1", ".mbr"}


def main() -> int:
    from PIL import Image  # only the importer needs a PNG encoder

    mui = Path.home() / "src" / "MUI" / "MUI" / "Images" / "xen"
    if not mui.is_dir():
        pins.report(False, "the MUI package (Images/xen) was not found")
        return 1

    out_root = pins.ROOT / "libs" / "hosted" / "aegir-trinket" / "resources" / "themes" / "xen"
    written = 0
    skipped = 0
    for src in sorted(mui.rglob("*")):
        if src.suffix.lower() not in SUFFIXES:
            continue
        try:
            width, height, pixels, _palette = decode(src)
        except IlbmError as problem:
            print(f"skip {src.name}: {problem}", file=sys.stderr)
            skipped += 1
            continue
        out = (out_root / src.relative_to(mui).parent /
               f"{src.stem}.{src.suffix[1:]}.png")
        out.parent.mkdir(parents=True, exist_ok=True)
        image = Image.new("RGBA", (width, height))
        image.putdata(pixels)
        image.save(out)
        written += 1

    print(f"imported {written} sprites into {out_root.relative_to(pins.ROOT)}"
          + (f", skipped {skipped}" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
