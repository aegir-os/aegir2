#!/usr/bin/env python3
"""Run Sys:S/network.manifest's parser against its host conformance cases.

    python3 scripts/check_netmanifest.py

Compiles apps/freestanding/aegir-net-config/src/manifest.cc with the host
compiler, against a stub of the one resolver function it uses
(scripts/netmanifest_stub/aegir/resolve.h) and
scripts/netmanifest_conformance.cc, then runs the assertions. The parser owns
the schema's keys, the loud refusals, the `hostname`-before-sections rule, and
the streaming that lets a heap-less command read the file at all; this proves
those in seconds, without a boot (specs/net.md).

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

SRC = pins.ROOT / "apps" / "freestanding" / "aegir-net-config" / "src"
STUB = pins.ROOT / "scripts" / "netmanifest_stub"
DRIVER = pins.ROOT / "scripts" / "netmanifest_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-netmanifest-") as scratch:
        binary = Path(scratch) / "netmanifest_conformance"
        compile_command = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            # The stub shadows <aegir/resolve.h>; it must come first.
            "-I",
            str(STUB),
            "-I",
            str(SRC),
            str(DRIVER),
            str(SRC / "manifest.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the netmanifest conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the network manifest parser")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
