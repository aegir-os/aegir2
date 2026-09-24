#!/usr/bin/env python3
"""Run the toolkit's UAX #9 against the Unicode conformance suites.

    python3 scripts/check_bidi.py

Compiles libs/hosted/aegir-trinket/src/bidi.cc with the host compiler (it is
plain C++), the tables scripts/gen_bidi_tables.py generates from the pinned
Unicode Character Database, and scripts/bidi_conformance.cc, then runs both of
Unicode's test files. Conformance is whole-file and exact: every case in
BidiTest.txt and BidiCharacterTest.txt must pass (specs/locale.md). The UCD
tree is the one `make deps` fetched (manifests/sources.toml), so the algorithm
is tested against the same Unicode version its tables were built from.

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
import pins

UCD = pins.ROOT / "projects" / "ucd" / "unicode-16.0.0"
TRINKET = pins.ROOT / "libs" / "hosted" / "aegir-trinket"
DRIVER = pins.ROOT / "scripts" / "bidi_conformance.cc"

SUITES = (("classes", "BidiTest.txt"), ("chars", "BidiCharacterTest.txt"))


def main() -> int:
    if not UCD.is_dir():
        pins.report(False, "the Unicode data is missing", "run: make deps")
        return 1
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-bidi-") as scratch:
        tables = Path(scratch) / "bidi_tables.cc"
        gen_bidi_tables.emit(UCD, tables)
        binary = Path(scratch) / "bidi_conformance"
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
            str(tables),
            str(TRINKET / "src" / "bidi.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ok = True
        for mode, filename in SUITES:
            ran = subprocess.run(
                [str(binary), mode, str(UCD / filename)],
                capture_output=True,
                text=True,
            )
            print(ran.stdout.strip(), flush=True)
            if ran.stderr.strip():
                sys.stderr.write(ran.stderr)
            pins.report(ran.returncode == 0, f"UAX #9 conformance: {filename}")
            ok = ok and ran.returncode == 0
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
