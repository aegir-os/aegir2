#!/usr/bin/env python3
"""Run the JPEG decoder against its host conformance cases.

    python3 scripts/check_jpeg.py

Builds the vendored libjpeg-turbo for the host (cached under out/check-jpeg, so
only the first run pays for it), compiles libs/hosted/aegir-jpeg/src/jpeg.cc and
scripts/jpeg_conformance.cc with the host compiler, and runs the assertions. The
two reference JPEGs are embedded in the driver as bytes, written by an encoder
other than the one under test, so a foreign file is decoded, not only a round
trip; what is pinned is the normalisation the decoder states (every colour type
as RGB) and the frame it serves (row order, a width*3 stride, no palette).

Host tools only (python3, a C++ compiler, cmake); not part of any target build.
Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pins

PROTO = pins.ROOT / "libs" / "freestanding" / "aegir-datatypes"
DATATYPES = pins.ROOT / "libs" / "hosted" / "aegir-datatypes"
JPEG_LIB = pins.ROOT / "libs" / "hosted" / "aegir-jpeg"
JPEG_SRC = pins.ROOT / "projects" / "libjpeg-turbo"
DRIVER = pins.ROOT / "scripts" / "jpeg_conformance.cc"
BUILD = pins.ROOT / "out" / "check-jpeg"


def host_env() -> dict[str, str]:
    """The environment a host cmake runs in.

    cmake and ninja are Aegir's pinned tools, installed by `make tools` into
    third_party/tools/venv/bin -- not necessarily on a bare PATH. The host C
    compiler is whatever `c++` the host has.
    """
    env = dict(os.environ)
    for tools_bin in (pins.TOOLS_ROOT / "venv" / "bin", pins.TOOLS_ROOT / "bin"):
        if tools_bin.is_dir():
            env["PATH"] = str(tools_bin) + os.pathsep + env.get("PATH", "")
    return env


def find_cmake() -> str | None:
    venv_cmake = pins.TOOLS_ROOT / "venv" / "bin" / "cmake"
    if venv_cmake.is_file():
        return str(venv_cmake)
    return shutil.which("cmake")


def host_libjpeg(prefix: Path) -> Path | None:
    """The host libjpeg-turbo static archive under `prefix`, if built."""
    for name in ("libjpeg.a", "libjpeg-static.a"):
        archive = prefix / "lib" / name
        if archive.is_file():
            return archive
    return None


def build_host_deps() -> Path | None:
    """Build the vendored libjpeg-turbo for the host, cached under out/."""
    prefix = BUILD / "prefix"
    if host_libjpeg(prefix) is not None:
        return prefix
    if not (JPEG_SRC / "CMakeLists.txt").is_file():
        pins.report(False, "the vendored libjpeg-turbo is not fetched", "run: make deps")
        return None
    cmake = find_cmake()
    if cmake is None:
        pins.report(False, "cmake is required to build the host libjpeg-turbo")
        return None
    env = host_env()

    BUILD.mkdir(parents=True, exist_ok=True)
    jbuild = BUILD / "libjpeg-build"
    steps = [
        [cmake, "-G", "Ninja", "-S", str(JPEG_SRC), "-B", str(jbuild),
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DENABLE_SHARED=OFF", "-DENABLE_STATIC=ON",
         "-DWITH_SIMD=OFF", "-DWITH_TURBOJPEG=OFF", "-DWITH_TOOLS=OFF",
         "-DWITH_TESTS=OFF", "-DWITH_FUZZ=OFF",
         f"-DCMAKE_INSTALL_PREFIX={prefix}"],
        [cmake, "--build", str(jbuild), "--target", "install"],
    ]
    for command in steps:
        result = subprocess.run(command, capture_output=True, text=True, env=env)
        if result.returncode != 0:
            pins.report(False, "the host libjpeg-turbo would not build")
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            return None
    if host_libjpeg(prefix) is None:
        pins.report(False, "the host libjpeg-turbo built no archive")
        return None
    return prefix


def main() -> int:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        pins.report(False, "no C++ compiler found")
        return 1

    prefix = build_host_deps()
    if prefix is None:
        return 1
    archive = host_libjpeg(prefix)
    assert archive is not None

    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / "jpeg_conformance"
    compile_command = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I", str(PROTO / "include"),
        "-I", str(DATATYPES / "include"),
        "-I", str(JPEG_LIB / "include"),
        "-I", str(prefix / "include"),
        str(DRIVER),
        str(JPEG_LIB / "src" / "jpeg.cc"),
        str(archive),
        "-lm",
        "-o", str(binary),
    ]
    result = subprocess.run(compile_command, capture_output=True, text=True)
    if result.returncode != 0:
        pins.report(False, "the JPEG conformance driver would not compile")
        sys.stderr.write(result.stderr)
        return 1

    ran = subprocess.run([str(binary)], capture_output=True, text=True)
    print(ran.stdout.strip(), flush=True)
    if ran.stderr.strip():
        sys.stderr.write(ran.stderr)
    pins.report(ran.returncode == 0, "the JPEG decoder")
    return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
