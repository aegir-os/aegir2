#!/usr/bin/env python3
"""Run aegir::args against its host conformance cases.

    python3 scripts/check_args.py

Compiles libs/freestanding/aegir-args/src/args.cc with the host compiler and
scripts/args_conformance.cc, then runs the assertions. The parser is a pure
value -- no allocation, no libc++, no seL4 -- so template reading, positional
order, and the /M item's reservation of a later positional are asserted
exactly (specs/dos.md).

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

ARGS = pins.ROOT / "libs" / "freestanding" / "aegir-args"
DRIVER = pins.ROOT / "scripts" / "args_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-args-") as scratch:
        binary = Path(scratch) / "args_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ARGS / "include"),
            str(DRIVER),
            str(ARGS / "src" / "args.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the args conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the aegir::args parser")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
