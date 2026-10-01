#!/usr/bin/env python3
"""Run aegir-mem's allocator against its host conformance cases.

    python3 scripts/check_allocator.py

Compiles libs/freestanding/aegir-mem/src/allocator.cc with the host compiler,
against a stub of the seL4 surface the allocator uses
(scripts/allocator_stub/sel4/sel4.h), and scripts/allocator_conformance.cc,
then runs the assertions. The allocator's splitting, buddy merge and free
lists are pure bookkeeping; only the kernel's retype rule -- a retype from a
spent untyped is refused -- is modelled, so the workload can prove in seconds
that no retype is ever made from a piece whose memory is already gone
(specs/memory.md).

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

MEM = pins.ROOT / "libs" / "freestanding" / "aegir-mem"
STUB = pins.ROOT / "scripts" / "allocator_stub"
DRIVER = pins.ROOT / "scripts" / "allocator_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-allocator-") as scratch:
        binary = Path(scratch) / "allocator_conformance"
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
            str(MEM / "include"),
            str(DRIVER),
            str(MEM / "src" / "allocator.cc"),
            str(MEM / "src" / "frame_region.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the allocator conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the aegir-mem allocator")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
