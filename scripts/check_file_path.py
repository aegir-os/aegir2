#!/usr/bin/env python3
"""Run the file requester's path and size helpers against their host cases.

    python3 scripts/check_file_path.py

Compiles libs/hosted/aegir-trinket/src/file_path.cc with
scripts/file_path_conformance.cc and runs the assertions. The helpers are pure
over their own strings -- no theme, no VFS -- so the host compiler and nothing
else is enough (specs/trinket/file_requester.md).

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
DRIVER = pins.ROOT / "scripts" / "file_path_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-file-path-") as scratch:
        binary = Path(scratch) / "file_path_conformance"
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
            str(TRINKET / "src" / "file_path.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the file-path check would not compile")
            sys.stderr.write(result.stderr)
            return 1
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(ran.stdout)
        sys.stderr.write(ran.stderr)
        ok = ran.returncode == 0

    pins.report(ok, "the file requester's path and size")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
