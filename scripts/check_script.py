#!/usr/bin/env python3
"""Run aegir::script against its host conformance cases.

    python3 scripts/check_script.py

Compiles libs/hosted/aegir-script/src/interpreter.cc with the host compiler and
scripts/script_conformance.cc, then runs the assertions. The command-file
parser and the frame stack are values -- no seL4, no allocation policy -- so
comment/blank stripping, the frame order, the cycle guard and the fail level
are asserted exactly (specs/shell.md).

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

SCRIPT = pins.ROOT / "libs" / "hosted" / "aegir-script"
DRIVER = pins.ROOT / "scripts" / "script_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-script-") as scratch:
        binary = Path(scratch) / "script_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(SCRIPT / "include"),
            str(DRIVER),
            str(SCRIPT / "src" / "interpreter.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the script conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the aegir::script interpreter core")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
