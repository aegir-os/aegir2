#!/usr/bin/env python3
"""Run the heap's free-region list against its host conformance cases.

    python3 scripts/check_regions.py

Compiles libs/hosted/aegir-heap/src/regions.h with the host compiler and
scripts/regions_conformance.cc, then runs the assertions. The list an `munmap`
released region joins is pure address arithmetic over caller-owned nodes -- an
address-ordered walk, a first fit, a split, a neighbour merge and a node pool
that grows through a source -- so it is asserted exactly (specs/memory.md). The
heap itself is the boot's to prove.

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

HEAP = pins.ROOT / "libs" / "hosted" / "aegir-heap"
DRIVER = pins.ROOT / "scripts" / "regions_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-regions-") as scratch:
        binary = Path(scratch) / "regions_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(HEAP / "src"),
            str(DRIVER),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the free-region conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the heap's free regions")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
