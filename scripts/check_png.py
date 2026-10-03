#!/usr/bin/env python3
"""Run the PNG decoder against its host conformance cases.

    python3 scripts/check_png.py

Builds the vendored zlib and libpng for the host (cached under out/check-png,
so only the first run pays for it), compiles libs/hosted/aegir-png/src/png.cc
and scripts/png_conformance.cc with the host compiler, and runs the assertions.
The PNGs are built in the driver as bytes -- signature, IHDR, PLTE, a deflate
IDAT, IEND -- so no file, no service and no encoder are needed; what is pinned
is the normalisation the decoder states (every colour type as RGBA) and the
frame it serves (row order, a width*4 stride, no palette).

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
PNG_LIB = pins.ROOT / "libs" / "hosted" / "aegir-png"
ZLIB_SRC = pins.ROOT / "projects" / "zlib"
PNG_SRC = pins.ROOT / "projects" / "libpng"
DRIVER = pins.ROOT / "scripts" / "png_conformance.cc"
BUILD = pins.ROOT / "out" / "check-png"


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


def host_libpng(prefix: Path) -> Path | None:
    """The host libpng static archive under `prefix`, if it has been built."""
    for name in ("libpng16.a", "libpng.a", "liblibpng16_static.a"):
        archive = prefix / "lib" / name
        if archive.is_file():
            return archive
    return None


def build_host_deps() -> Path | None:
    """Build the vendored zlib and libpng for the host, cached under out/."""
    prefix = BUILD / "prefix"
    if host_libpng(prefix) is not None:
        return prefix
    if not (ZLIB_SRC / "CMakeLists.txt").is_file() or not (PNG_SRC / "CMakeLists.txt").is_file():
        pins.report(False, "the vendored zlib and libpng are not fetched", "run: make deps")
        return None
    cmake = find_cmake()
    if cmake is None:
        pins.report(False, "cmake is required to build the host zlib/libpng")
        return None
    env = host_env()

    BUILD.mkdir(parents=True, exist_ok=True)
    zbuild = BUILD / "zlib-build"
    pbuild = BUILD / "libpng-build"
    steps = [
        [cmake, "-G", "Ninja", "-S", str(ZLIB_SRC), "-B", str(zbuild),
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DBUILD_SHARED_LIBS=OFF",
         "-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_STATIC=ON",
         "-DZLIB_BUILD_TESTING=OFF", f"-DCMAKE_INSTALL_PREFIX={prefix}"],
        [cmake, "--build", str(zbuild), "--target", "install"],
        [cmake, "-G", "Ninja", "-S", str(PNG_SRC), "-B", str(pbuild),
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DBUILD_SHARED_LIBS=OFF",
         "-DPNG_SHARED=OFF", "-DPNG_STATIC=ON", "-DPNG_TESTS=OFF",
         "-DPNG_TOOLS=OFF", "-DPNG_EXECUTABLES=OFF",
         "-DPNG_HARDWARE_OPTIMIZATIONS=OFF",
         f"-DZLIB_INCLUDE_DIR={prefix / 'include'}",
         f"-DZLIB_LIBRARY={prefix / 'lib' / 'libz.a'}",
         f"-DCMAKE_INSTALL_PREFIX={prefix}"],
        [cmake, "--build", str(pbuild), "--target", "install"],
    ]
    for command in steps:
        result = subprocess.run(command, capture_output=True, text=True, env=env)
        if result.returncode != 0:
            pins.report(False, "the host zlib/libpng would not build")
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            return None
    if host_libpng(prefix) is None:
        pins.report(False, "the host libpng built no archive")
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
    archive = host_libpng(prefix)
    assert archive is not None

    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / "png_conformance"
    compile_command = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I", str(PROTO / "include"),
        "-I", str(DATATYPES / "include"),
        "-I", str(PNG_LIB / "include"),
        "-I", str(prefix / "include"),
        str(DRIVER),
        str(PNG_LIB / "src" / "png.cc"),
        str(archive),
        str(prefix / "lib" / "libz.a"),
        "-lm",
        "-o", str(binary),
    ]
    result = subprocess.run(compile_command, capture_output=True, text=True)
    if result.returncode != 0:
        pins.report(False, "the PNG conformance driver would not compile")
        sys.stderr.write(result.stderr)
        return 1

    ran = subprocess.run([str(binary)], capture_output=True, text=True)
    print(ran.stdout.strip(), flush=True)
    if ran.stderr.strip():
        sys.stderr.write(ran.stderr)
    pins.report(ran.returncode == 0, "the PNG decoder")
    return 0 if ran.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
