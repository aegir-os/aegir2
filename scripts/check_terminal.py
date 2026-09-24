#!/usr/bin/env python3
"""Run the terminal's text grid against its host conformance cases.

    python3 scripts/check_terminal.py

Compiles libs/hosted/aegir-trinket/src/terminal_buffer.cc with the host
compiler, the UAX #9 tables and character-width table the pinned Unicode
Character Database generates (scripts/gen_bidi_tables.py,
scripts/gen_width_tables.py), bidi.cc and unicode.cc, and
scripts/terminal_conformance.cc, then runs the assertions. The grid is a pure
value -- no pixels, no seL4 -- so its cell placement, wrapping, scrollback,
wide and combining characters, and BiDi reordering are asserted exactly
(specs/terminal.md).

Host tools only (python3 and a C++ compiler); not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import gen_bidi_tables
import gen_width_tables
import pins

UCD = pins.ROOT / "projects" / "ucd" / "unicode-16.0.0"
TRINKET = pins.ROOT / "libs" / "hosted" / "aegir-trinket"
DRIVER = pins.ROOT / "scripts" / "terminal_conformance.cc"


def main() -> int:
    if not UCD.is_dir():
        pins.report(False, "the Unicode data is missing", "run: make deps")
        return 1
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-terminal-") as scratch:
        bidi_tables = Path(scratch) / "bidi_tables.cc"
        width_tables = Path(scratch) / "width_tables.cc"
        gen_bidi_tables.emit(UCD, bidi_tables)
        gen_width_tables.emit(UCD, width_tables)
        binary = Path(scratch) / "terminal_conformance"
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
            str(bidi_tables),
            str(width_tables),
            str(TRINKET / "src" / "terminal_buffer.cc"),
            str(TRINKET / "src" / "bidi.cc"),
            str(TRINKET / "src" / "unicode.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the terminal conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the terminal text grid")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
