#!/usr/bin/env python3
"""Render the toolkit's gadgets on the host and lay them beside the reference.

    python3 scripts/check_theme.py

Compiles scripts/theme_conformance.cc with the toolkit's theme.cc, theme_xen.cc,
canvas.cc and unicode.cc and the host compiler, runs it, and writes the render
to out/theme/preview.png (twice size, so the 1px bevels are legible). Beside it
goes out/theme/reference.png: the imported XEN artwork at the same scale, for
the eye to compare against. widget.cc's damage path names Window, so the sel4
stub the layout check uses shadows that header here too.

This is the fast loop (specs/trinket/theme-xen.md): a gadget change is seen in
seconds instead of a ten-minute boot, and a wrong bevel is visible rather than
guessed.

Host tools only (python3 and a C++ compiler); not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pins

TRINKET = pins.ROOT / "libs" / "hosted" / "aegir-trinket"
CONSOLE = pins.ROOT / "libs" / "freestanding" / "aegir-console"
IPC = pins.ROOT / "libs" / "freestanding" / "aegir-ipc"
INPUT = pins.ROOT / "libs" / "freestanding" / "aegir-input"
STUB = pins.ROOT / "scripts" / "layout_stub"
DRIVER = pins.ROOT / "scripts" / "theme_conformance.cc"
OUT = pins.ROOT / "out" / "theme"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    ppm = OUT / "render.ppm"
    with tempfile.TemporaryDirectory(prefix="aegir-theme-") as scratch:
        binary = Path(scratch) / "theme_conformance"
        generated = Path(scratch) / "theme_data.cc"
        gen = subprocess.run(
            [sys.executable, str(pins.ROOT / "scripts" / "gen_theme.py"), str(generated)],
            capture_output=True, text=True)
        if gen.returncode != 0:
            pins.report(False, "the theme data would not generate")
            sys.stderr.write(gen.stderr)
            return 1
        compile_command = [
            compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(STUB),
            "-I", str(TRINKET / "include"),
            "-I", str(TRINKET / "src"),
            "-I", str(CONSOLE / "include"),
            "-I", str(IPC / "include"),
            "-I", str(INPUT / "include"),
            str(DRIVER),
            str(TRINKET / "src" / "theme.cc"),
            str(TRINKET / "src" / "theme_xen.cc"),
            str(TRINKET / "src" / "canvas.cc"),
            str(TRINKET / "src" / "unicode.cc"),
            str(generated),
            "-o", str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the theme render would not compile")
            sys.stderr.write(result.stderr)
            return 1
        ran = subprocess.run([str(binary), str(ppm)], capture_output=True, text=True)
        if ran.returncode != 0:
            pins.report(False, "the theme render did not run")
            sys.stderr.write(ran.stderr)
            return 1

    from PIL import Image

    scale = 2
    render = Image.open(ppm).convert("RGB")
    render.resize((render.width * scale, render.height * scale), Image.NEAREST).save(
        OUT / "preview.png"
    )

    # The imported XEN artwork, at the same scale, in a row for the eye.
    art = TRINKET / "resources" / "themes" / "xen" / "Plain" / "11pt"
    names = ["ArrowUp.mf0.png", "ArrowDown.mf0.png", "CheckMark.mf0.png",
             "CheckMark.mf1.png", "RadioButton.mf1.png", "PopUp.mf0.png"]
    sprites = [Image.open(art / n).convert("RGBA") for n in names if (art / n).exists()]
    if sprites:
        strip = Image.new("RGBA", (sum(s.width for s in sprites) + 6 * (len(sprites) - 1),
                                   max(s.height for s in sprites)), (170, 170, 170, 255))
        x = 0
        for s in sprites:
            strip.alpha_composite(s, (x, 0))
            x += s.width + 6
        strip.resize((strip.width * scale, strip.height * scale), Image.NEAREST).save(
            OUT / "reference.png"
        )

    # A non-degenerate render: the background is one colour; the gadgets must
    # add several more (a face, a highlight, a shadow, black outlines).
    colors = render.getcolors(maxcolors=1 << 24) or []
    ok = len(colors) >= 4
    pins.report(ok, "the theme render")
    print(f"    wrote {OUT.relative_to(pins.ROOT)}/preview.png "
          f"({render.width}x{render.height}, {len(colors)} colours)", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
