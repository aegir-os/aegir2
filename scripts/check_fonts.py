#!/usr/bin/env python3
"""Run the toolkit's font catalog against its host conformance cases.

    python3 scripts/check_fonts.py

Compiles libs/hosted/aegir-trinket/src/fonts.cc with the host compiler and
scripts/fonts_conformance.cc, then runs the assertions. The two halves the
catalog needs no filesystem for -- reading a BDF header for a face's own
name, and choosing a face from a list -- are pure, so they are asserted
exactly (specs/fonts.md). The scan itself is the boot's to prove.

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
DRIVER = pins.ROOT / "scripts" / "fonts_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-fonts-") as scratch:
        binary = Path(scratch) / "fonts_conformance"
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
            str(TRINKET / "src" / "fonts.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the font conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the font catalog")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
