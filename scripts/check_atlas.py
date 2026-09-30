#!/usr/bin/env python3
"""Run the toolkit's glyph layout against its host conformance cases.

    python3 scripts/check_atlas.py

Compiles libs/hosted/aegir-trinket/src/font.cc and canvas.cc (with unicode.cc,
which font.cc's measurement path converts through) and
scripts/atlas_conformance.cc with the host compiler, then runs the assertions.

The atlas is where a glyph the font service rasterized lands and where the
canvas reads it back, and the pen is how far the canvas moves between glyphs, so
the two are the one thing a served face cannot get wrong quietly: a copy that
scans the wrong stride, keeps the wrong row, or does not move what a grown atlas
moved draws a smudge, and a glyph with no box that does not advance runs the
words together -- and both draw on a screen nobody is asserting. The cases are
pure -- synthetic rasters, the canvas's own read expression, a pixel buffer --
so no filesystem and no rasterizer is needed (specs/fonts.md).

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
DRIVER = pins.ROOT / "scripts" / "atlas_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-atlas-") as scratch:
        binary = Path(scratch) / "atlas_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(TRINKET / "include"),
            "-I",
            str(TRINKET / "src"),
            str(DRIVER),
            str(TRINKET / "src" / "font.cc"),
            str(TRINKET / "src" / "canvas.cc"),
            str(TRINKET / "src" / "unicode.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the atlas conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the glyph atlas")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
