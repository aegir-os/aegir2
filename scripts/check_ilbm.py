#!/usr/bin/env python3
"""Run the ILBM decoder against its host conformance cases.

    python3 scripts/check_ilbm.py

Compiles libs/hosted/aegir-ilbm/src/ilbm.cc and scripts/ilbm_conformance.cc
with the host compiler, then runs the assertions. The images are built in the
driver as IFF bytes -- BMHD, CMAP and BODY -- so no file, no service and no
vendored tree is needed; what is pinned is the arithmetic that goes wrong
quietly: the big-endian fields, the per-plane word-aligned row, ByteRun1 and
the palette (specs/datatypes.md).

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

ILBM = pins.ROOT / "libs" / "hosted" / "aegir-ilbm"
DATATYPES = pins.ROOT / "libs" / "hosted" / "aegir-datatypes"
PROTO = pins.ROOT / "libs" / "freestanding" / "aegir-datatypes"
DRIVER = pins.ROOT / "scripts" / "ilbm_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-ilbm-") as scratch:
        binary = Path(scratch) / "ilbm_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(PROTO / "include"),
            "-I",
            str(DATATYPES / "include"),
            "-I",
            str(ILBM / "include"),
            str(DRIVER),
            str(ILBM / "src" / "ilbm.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the ILBM conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the ILBM decoder")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
