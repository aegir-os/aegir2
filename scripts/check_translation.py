#!/usr/bin/env python3
"""Run the toolkit's gettext .mo parser against in-memory images.

    python3 scripts/check_translation.py

Compiles libs/hosted/aegir-trinket/src/translation.cc with the host compiler
and runs scripts/translation_conformance.cc, which builds .mo images in memory
-- both byte orders, contexts, plurals and a three-form catalogue -- so no
catalogue tool or file on disk is needed. The parser uses only the standard
library, so it compiles host-side as well as on target.

Host tools only (python3 and a C++ compiler); not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import compile_po
import pins

TRINKET = pins.ROOT / "libs" / "hosted" / "aegir-trinket"
CATALOGUE = TRINKET / "resources" / "translations" / "trinket.po"
DRIVER = pins.ROOT / "scripts" / "translation_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-translation-") as scratch:
        data = Path(scratch) / "translation_data.cc"
        compile_po.emit(CATALOGUE, data)
        binary = Path(scratch) / "translation_conformance"
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
            str(TRINKET / "src" / "translation.cc"),
            str(data),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the translation conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "gettext .mo parsing")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
