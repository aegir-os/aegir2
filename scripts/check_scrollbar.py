#!/usr/bin/env python3
"""Run the scrollbar's thumb geometry against its host cases.

    python3 scripts/check_scrollbar.py

Compiles libs/hosted/aegir-trinket/src/scrollbar.cc with widget.cc, canvas.cc
and unicode.cc and scripts/scrollbar_conformance.cc with the host compiler, then
runs the assertions. widget.cc's damage path names Window, whose header includes
<aegir/console.h> and so <sel4/sel4.h>, so the check shadows that header with
scripts/layout_stub/sel4/sel4.h, the same stub the layout check uses.

The thumb is a mapping from content and page to a position and length, and the
drag is its inverse; it is pure, so synthetic numbers are enough
(specs/trinket/scrollbar.md).

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
DRIVER = pins.ROOT / "scripts" / "scrollbar_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-scrollbar-") as scratch:
        binary = Path(scratch) / "scrollbar_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            # The stub shadows <sel4/sel4.h>; it must come first.
            "-I",
            str(STUB),
            "-I",
            str(TRINKET / "include"),
            "-I",
            str(TRINKET / "src"),
            "-I",
            str(CONSOLE / "include"),
            "-I",
            str(IPC / "include"),
            "-I",
            str(INPUT / "include"),
            str(DRIVER),
            str(TRINKET / "src" / "scrollbar.cc"),
            str(TRINKET / "src" / "widget.cc"),
            str(TRINKET / "src" / "canvas.cc"),
            str(TRINKET / "src" / "unicode.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the scrollbar conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the scrollbar thumb geometry")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
