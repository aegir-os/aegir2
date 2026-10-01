#!/usr/bin/env python3
"""Run the popup anchor rule against its host cases.

    python3 scripts/check_popup.py

Compiles libs/hosted/aegir-trinket/src/popup.cc and scripts/popup_conformance.cc
with the host compiler and runs the assertions. The rule draws nothing and names
no widget, so this is the lightest of the checks: no stub, no canvas.

Where a popup goes -- below the widget that opened it, above it when it does not
fit below, inside the bounds either way -- is pure arithmetic, so synthetic
numbers are enough (specs/trinket/popup.md).

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
DRIVER = pins.ROOT / "scripts" / "popup_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-popup-") as scratch:
        binary = Path(scratch) / "popup_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(TRINKET / "include"),
            str(DRIVER),
            str(TRINKET / "src" / "popup.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the popup conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the popup anchor rule")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
