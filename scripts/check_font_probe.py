#!/usr/bin/env python3
"""Run the font service's probe against the faces the vendored trees hold.

    python3 scripts/check_font_probe.py

Compiles apps/hosted/aegir-font/src/probe.cc with the host compiler and
scripts/probe_conformance.cc, then drives the assertions over the real BDF,
TrueType and collection files `Sys:Fonts` is packed from (specs/fonts.md). The
probe reads a face's own name without a rasterizer, and the files are the ones
the system volume ships, so this is the index's proof without a boot.

The vendored trees are needed (make deps); the check fails loudly when they are
missing rather than passing vacuously.

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

FONT = pins.ROOT / "apps" / "hosted" / "aegir-font"
DRIVER = pins.ROOT / "scripts" / "probe_conformance.cc"
FACES = [
    "projects/terminus-font/ter-u12n.bdf",
    "projects/noto-fonts/hinted/ttf/NotoSans/NotoSans-Regular.ttf",
    "projects/noto-fonts/archive/unhinted/NotoSansCJK/NotoSansCJK-Regular.ttc",
]


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    missing = [face for face in FACES if not (pins.ROOT / face).is_file()]
    if missing:
        pins.report(False, "the vendored faces are not fetched", f"run: make deps ({missing[0]})")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-font-probe-") as scratch:
        binary = Path(scratch) / "probe_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(FONT / "src"),
            str(DRIVER),
            str(FONT / "src" / "probe.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the probe conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary), str(pins.ROOT)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the font probe")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
