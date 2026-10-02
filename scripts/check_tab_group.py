#!/usr/bin/env python3
"""Run the tab group's strip layout against its host cases.

    python3 scripts/check_tab_group.py

Compiles libs/hosted/aegir-trinket/src/tab_group.cc with widget.cc, canvas.cc
and unicode.cc and scripts/tab_group_conformance.cc with the host compiler, then
runs the assertions. widget.cc's damage path names Window, whose header includes
<aegir/console.h> and so <sel4/sel4.h>, so the check shadows that header with
scripts/layout_stub/sel4/sel4.h, the same stub the layout check uses.

The strip layout and `tab_at` are pure -- no font, no theme -- so synthetic
numbers are enough (specs/trinket/tabs.md).

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
DRIVER = pins.ROOT / "scripts" / "tab_group_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-tab-group-") as scratch:
        binary = Path(scratch) / "tab_group_conformance"
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
            str(TRINKET / "src" / "tab_group.cc"),
            str(TRINKET / "src" / "widget.cc"),
            str(TRINKET / "src" / "canvas.cc"),
            str(TRINKET / "src" / "unicode.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the tab-group check would not compile")
            sys.stderr.write(result.stderr)
            return 1
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(ran.stdout)
        sys.stderr.write(ran.stderr)
        ok = ran.returncode == 0

    pins.report(ok, "the tab group's strip layout")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
