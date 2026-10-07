#!/usr/bin/env python3
"""Run the POSIX path view against its host conformance cases.

    python3 scripts/check_posix_path.py

Compiles libs/aegir-posix/include/aegir/posix/path.h with the host compiler and
scripts/posix_path_conformance.cc, then runs the assertions. The translation is
a pure function -- the spec's table, the `.`/`..` normalization, the native path
that passes through untouched, and the reverse `getcwd` and a listing present --
so it is asserted exactly (specs/posix.md). The view end to end, over the
namespace and the volume protocol, is the acceptance's to prove.

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

POSIX = pins.ROOT / "libs" / "aegir-posix"
DRIVER = pins.ROOT / "scripts" / "posix_path_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-posix-path-") as scratch:
        binary = Path(scratch) / "posix_path_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(POSIX / "include"),
            str(DRIVER),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the path-view conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the POSIX path view")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
