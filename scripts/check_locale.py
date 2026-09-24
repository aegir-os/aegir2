#!/usr/bin/env python3
"""Run the toolkit's Locale formatting against the generated CLDR data.

    python3 scripts/check_locale.py

Compiles libs/hosted/aegir-trinket/src/locale.cc with the host compiler, the
.locale blobs scripts/gen_locale_data.py generates from the pinned CLDR, and
scripts/locale_conformance.cc, then runs the assertions. The UCD-free half of
the toolkit compiles host-side; only <string>, <vector> and <fstream> are used,
and neither the kernel nor musl is involved.

Host tools only (python3 and a C++ compiler); not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import gen_locale_data
import pins

CLDR = pins.ROOT / "projects" / "cldr" / "46.0.0"
MANIFEST = pins.ROOT / "manifests" / "locales.toml"
TRINKET = pins.ROOT / "libs" / "hosted" / "aegir-trinket"
DRIVER = pins.ROOT / "scripts" / "locale_conformance.cc"


def main() -> int:
    if not CLDR.is_dir():
        pins.report(False, "the CLDR data is missing", "run: make deps")
        return 1
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-locale-") as scratch:
        tables = Path(scratch) / "locale_data.cc"
        files = Path(scratch) / "locales"
        gen_locale_data.emit(CLDR, MANIFEST, tables, files)
        binary = Path(scratch) / "locale_conformance"
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
            str(TRINKET / "src" / "locale.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the locale conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary), str(files)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "locale formatting against the CLDR data")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
