#!/usr/bin/env python3
"""Run the text document against its host cases.

    python3 scripts/check_text_document.py

Compiles libs/hosted/aegir-trinket/src/text_document.cc and
scripts/text_document_conformance.cc with the host compiler, then runs the
assertions. The model is pure -- no font, no theme, no widget -- so it needs no
stub and no target headers.

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
DRIVER = pins.ROOT / "scripts" / "text_document_conformance.cc"


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    with tempfile.TemporaryDirectory(prefix="aegir-text-document-") as scratch:
        binary = Path(scratch) / "text_document_conformance"
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
            str(TRINKET / "src" / "text_document.cc"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            pins.report(False, "the text document conformance driver would not compile")
            sys.stderr.write(result.stderr)
            return 1

        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip(), flush=True)
        if ran.stderr.strip():
            sys.stderr.write(ran.stderr)
        pins.report(ran.returncode == 0, "the text document")
        return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
