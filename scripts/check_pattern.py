#!/usr/bin/env python3
"""Run the AmigaDOS matcher against its host cases.

    python3 scripts/check_pattern.py

Compiles libs/freestanding/aegir-pattern/src/pattern.cc with
scripts/pattern_conformance.cc and runs the assertions. The matcher is pure --
no seL4, no libc, no allocation -- so the host compiler and nothing else is
enough.

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

PATTERN = pins.ROOT / "libs" / "freestanding" / "aegir-pattern"
DRIVER = pins.ROOT / "scripts" / "pattern_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-pattern-") as scratch:
        binary = Path(scratch) / "pattern_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(PATTERN / "include"),
            str(DRIVER),
            str(PATTERN / "src" / "pattern.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the pattern check would not compile")
            sys.stderr.write(result.stderr)
            return 1
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(ran.stdout)
        sys.stderr.write(ran.stderr)
        ok = ran.returncode == 0

    pins.report(ok, "the AmigaDOS matcher")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
